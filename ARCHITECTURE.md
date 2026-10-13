# ESPRack architecture

How the framework works. It's written for people who change the core or
write modules. The step-by-step module tutorial is
[`docs/module-guide.md`](docs/module-guide.md), and building a device
project is covered in [`docs/consumer-guide.md`](docs/consumer-guide.md).

Every statement here was checked against `feature/ses-light-support`.
References are `path:line` relative to the repo root unless they say
otherwise.

---

## 1. Layers

```
┌──────────────────────────────────────────────────────────────────────┐
│ Consumer project (e.g. esp-rack-light-demo)                          │
│   main.cpp: Builder chain   ·   app module(s)   ·   factory/feature  │
│   .ini defaults   ·   branding overrides                             │
├──────────────────────────────────────────────────────────────────────┤
│ modules/*  — one PlatformIO library each, opt-in via .install<>()    │
│   wifi ap security presence filesystem ntp ota upload-firmware       │
│   auto-update mqtt telegram discovery cert-manager mothership        │
│   wireguard system-status ws-diag config-manager-ui web-endpoints    │
│   features ui-dyn restart factory-reset   (+ template skeleton)      │
├──────────────────────────────────────────────────────────────────────┤
│ lib/ESPRack — core, hardware-independent                             │
│   Module/Builder/App · StatefulService · ConfigManager/Delegate      │
│   WebManager (manifest, typed features, actions, pages) · Form DSL   │
│   WsManager (single multiplexed socket) · HttpEndpoint · AdaptiveJson│
│   ProblemJson · DataApi · SecurityManager iface · SecretsVault       │
│   DeviceIdentity · HeapMonitor · TLSContextService · provider ifaces │
├──────────────────────────────────────────────────────────────────────┤
│ Arduino-ESP32 3.x / IDF 5 (pioarduino) · ESPAsyncWebServer/AsyncTCP  │
│ (ESP32Async) · ArduinoJson 6 · LittleFS                              │
└──────────────────────────────────────────────────────────────────────┘
```

**The rule:** the core contains only hardware-independent machinery:
lifecycle, persistence, web plumbing, security primitives, and
*interfaces/arbitration* for shared concepts. Anything that drives a
peripheral, a protocol or a network service is a module that the consumer
installs. When several modules must cooperate on one concept, the
interface goes into the core and each implementation stays in its module.
Messaging already works this way: `lib/ESPRack/include/messaging/IMessagingProvider.h`
is the core interface, and `modules/telegram` and `modules/mqtt` implement
it. Time works the same way (§2.7): core `time/TimeSync.h` owns the clock and
arbitrates sources; `modules/ntp` (and later GPS or RTC modules) submit samples.
`geo/ILocationProvider.h` is the location interface; no module implements it
yet.

The React UI (`ui/`) is part of the library. Consumers don't write
frontend code: every module describes its UI in C++, and the UI renders it
from the manifest (§4).

---

## 2. Composition and lifecycle

### 2.1 Builder → App

```cpp
app = ESPRack::Builder(&server, NAME, VERSION)
        .install<A>(args...)      // constructs A in place (Builder.h:40-46)
        .install<B>()
        .build();                 // sort + onInstall + hand over to App
app->begin();                     // setup()
app->loop();                      // loop()
```

`Builder::build()` (`lib/ESPRack/src/Builder.cpp:65-181`):

1. **Describe.** Calls `describe()` on every module, which fills in
   `ModuleDescriptor{id, version, requires_, priority, enabled}`
   (`Module.h:81-106`).
2. **Index.** Indexes modules by id and logs duplicate ids.
3. **Check dependencies.** Logs every `requires_` entry that isn't
   installed.
4. **Sort.** Runs Kahn's topological sort, picking the lowest
   `(priority, id)` among ready modules (`:97-133`). If the sort can't
   finish because of a cycle *or a missing dependency*, it logs
   "dependency cycle" and falls back to insertion order (`:135-142`).
   **Nothing aborts**, despite `Builder.h:85-90`.
5. **Install.** Constructs the `App` and runs `onInstall(ctx)` in sorted
   order. Modules with `enabled == false` are skipped (`:164-167`). After
   each install, `ctx.security` and `ctx.presence` are refreshed from the
   App (`:175-176`), so modules installed later see the real managers.

The presets `core()`, `networking()` and `full()` are still empty TODOs
(`Builder.cpp:28-38`). The knobs `bufferSize()`, `cors()` and
`wsKeepAlive()` (`Builder.h:71-82`) are stored but **never passed to the
App**. `App::begin()` hard-codes `beginPingPong(20, 60000)` (`App.cpp:270`).

### 2.2 The App constructor (before any `onInstall`)

`lib/ESPRack/src/App.cpp:49-131`:

- Builds `ConfigManager` on LittleFS (`ESPFS`).
- Builds `WsManager` with a 16-slot queue capacity (`:54-64`), and
  `WebManager` with `NullSecurityManager` as the initial security
  manager.
- Builds `TLSContextService`, which `app->tls()` returns.
- Publishes `DeviceIdentity` (project name, version, `FACTORY_HW_REVISION`)
  and prints the firmware tags (`[fw] base | flavor | version`).
- Registers the framework-owned compound feature **`system`**, the "System"
  menu shell at order 900. Modules add tabs to it with
  `addTabToFeature("system", …)`.

### 2.3 `App::begin()`

`App.cpp:141-410`, in order:

| Step | What happens |
|---|---|
| 0 | `WebManager::registerModule(id, version)` and `registerBuildFeature(id, true)` for every installed module, plus `project` = `FT_PROJECT`. The manifest's `modules[]` and `buildFeatures` mirror the `.install<>()` chain (`:192-200`). |
| 1 | Mount LittleFS **without format**, retry once, and format only as a last resort, logging loudly (`:218-230`). |
| 2 | `ConfigManager::begin()` sets up the `/config` work directories. |
| 3 | `WiFi.mode(WIFI_AP_STA)`: lwIP comes up before any module touches sockets. |
| 4 | `onBegin()` for every module in install order, logging `[boot] post-<id>: free=… max_alloc=…` after each (`:250-260`). |
| 5 | `WebManager::begin()` mounts `/api/v1/ui/manifest` and calls `registerEndpoints()` on every registered entry. **This is when module HTTP routes and WS channels come into existence.** |
| 6 | `WsManager::beginPingPong(20, 60000)`. |
| 7 | Static UI: PROGMEM `WWWData` routes with three cache tiers, or `/www` on LittleFS without `PROGMEM_WWW`. `onNotFound` returns a problem+json 404 for `/api/*`, the SPA shell for other GETs, and 200 for CORS preflight (`:283-362`). |
| 7.5 | Middleware raises every connection's rx timeout to 10 s and ack timeout to 10 s (`ESPRACK_HTTP_RX_TIMEOUT_S`, `_ACK_TIMEOUT_MS`). This stops the server from killing its own queued connections during first-load bursts on single-core C3/C6 (`:364-393`). |
| 7.6 | `DataApi::instance().mount()` mounts `/api/v1/data` (`:395-399`). |
| 8 | `server.begin()`. The consumer doesn't call it. |

### 2.4 `App::loop()` and shutdown

`App::loop()` (`App.cpp:412-485`):

- Runs `onLoop()` for every module, in install order.
- Then runs `wsManager_.processAllQueues()`.
- Optional instrumentation:
  - `FT_LOOP_PROFILE` logs per-module time, heap and stack deltas every 60 s.
  - `FT_HEAP_MONITOR` runs a 60 s heap-drift sampler and logs a "soak"
    snapshot every 5 min.

**Restart and shutdown** (LIB-FIX-006). Every built-in reboot path calls
`ESPRack::restartDevice(reason, afterShutdown)`:

1. A call from another task (HTTP handler, mothership task) hands the
   restart to `App::loop()`, so hooks never run concurrently with a module's
   `onLoop()`. If the loop doesn't respond within 3 s, the caller runs them
   itself.
2. `onShutdown()` runs in reverse install order, once, each module timed
   against `ESPRACK_SHUTDOWN_BUDGET_MS` (500 ms; overruns are logged).
3. The optional `afterShutdown` step runs. Factory reset wipes `/config`
   here, and the config restores write their files here, so a hook can't
   resurrect or overwrite them.
4. `esp_restart()`.

An `esp_register_shutdown_handler` hook covers reboots that bypass this,
such as ArduinoOTA's own. `ESPRack::restartReason()` tells a hook why it is
running.

### 2.5 Threads

| Context | Runs |
|---|---|
| `loopTask` (Arduino) | `setup()` (and therefore `build()`/`onInstall`/`begin()`/`onBegin`), `onLoop()`, `processAllQueues()` from `App::loop` |
| `async_tcp` task | every HTTP handler: feature form/state GET/POST, actions, page endpoints, the manifest, the static UI. Also the WS event handler, which **also calls `processAllQueues()`** (`WsManager.h:733`), so WS writes can be applied on this task too. |
| WiFi/lwIP/system event tasks | `WiFi.onEvent` callbacks (e.g. NTP's `configureNTP`) |
| lwIP `tcpip` thread | `sntp_sync_time()` — TimeSync's SNTP hook only stashes the sample for `loop()` |
| module-owned tasks | e.g. the mothership check-in task; see the module catalog |

`StatefulService<T>` holds a recursive mutex per service, and every
`read()`/`update*()` takes it (`StatefulService.h:129-139`). Modules must
reach shared state only through those calls and drive hardware only from
`onLoop()`.

### 2.6 Late-bound providers

`ModuleContext` (`Module.h:63-73`) carries `server`, `cfgMgr`, `web`, `ws`,
`security`, `presence`, `app`, `deviceName` and `deviceVersion`. Modules
that provide a cross-module service register it on the App during their
`onInstall`:

| Getter | Interface | Set by |
|---|---|---|
| `setSecurityManager` | `SecurityManager` | security |
| `setPresence` | `PresenceService` | presence |
| `setTelegram` | `ITelegramProvider` | telegram |
| `setMqtt` | `IMqttProvider` | mqtt |
| `setCert` | `ICertProvider` | cert-manager |
| `setMothership` / `setMothershipProfile` | `IMothershipProvider` / `IMothershipProfileProvider` | mothership |
| `setWireguard` | `IWireguardProvider` | wireguard |
| `setLocation` | `ILocationProvider` (`geo/ILocationProvider.h`) | a location module (none yet) |

`App.h:72-134` defines these. Consumers read them through `app->mqtt()` and
similar getters and must null-check. `app->tls()` is App-owned and always
set. Reading a provider in `onBegin()` is always safe. Reading it in
`onInstall()` works only if the provider's module was installed earlier.

Modules that install **before** `security` (priority 8) must not capture
`ctx.security`, because at that point it's still the `NullSecurityManager`,
which authenticates everyone as admin. `uiDyn` (6) and `presence` (7) used
to do exactly that (LIB-FIX-005). Now:

- presence resolves `app->security()` on every handshake;
- uiDyn takes the manager in `onBegin()`.

### 2.7 Time (`time/TimeSync.h`, SES-LIB-002)

`TimeSync` is a core singleton, always present. `App::begin()` starts it first
and `App::loop()` ticks it first.

- **Who may set the clock.** Sources offer samples; `TimeArbiter`
  (`time/TimeArbiter.h`, Arduino-free, host-tested in `test-host/`) decides.
  Rank NTP > GPS > external RTC > operator (Manual) > RtcHold, each with a
  freshness window, a plausibility window (2026..2099), per-source step
  thresholds, confirmation of jumps over 1 h, and a 2-of-3 dispute check (NTP
  vs a GPS that agrees with the RTC).
- **SNTP.** TimeSync defines the strong `sntp_sync_time()` (lwIP's is weak),
  so lwIP hands the server time to TimeSync instead of stepping the clock. An
  implausible NTP time never reaches the clock. A *disputed* one (NTP far from
  a GPS that agrees with the RTC) is still applied, since NTP ranks first:
  `disputed()` and the `Disputed` event let the application decide, e.g. to
  treat the time as invalid.
- **Validity.** `isValid()` = an accepted sample whose uncertainty
  (accuracy + 30 ppm drift) is under 10 min, and a plausible system clock.
  `clockSane()` (valid now, or valid earlier this boot, or no source
  registered and the clock is past 2026) gates JWT `iat/exp`, the TLS X.509
  time and certificate expiry; read `time(nullptr)` after it.
- **Soft resets.** A 32-byte `RTC_NOINIT` record lets the clock stay valid
  across software/panic/watchdog resets (RtcHold), for up to 7 days. Not
  after deep sleep: the RC slow clock drifts far more than 30 ppm.
- **Timezone.** TimeSync owns libc `TZ` (`setTimezone(posix, label)`,
  `tzRevision()`); the NTP module passes its setting through it.
- **Operator.** Admin action `system.setTime?epoch=<s>[&ms=<ms>]`, exposed as
  "Set time" on the NTP status tab.
- **Events.** `subscribe()` listeners get `BecameValid`, `BecameInvalid`,
  `Stepped` (valid before and after), `SourceChanged`, `Stale`, `Disputed`,
  `TzChanged`, always on the loop task.

---

## 3. State and persistence

### 3.1 `StatefulService<T>`

`lib/ESPRack/include/StatefulService.h`. One `T` per service holds settings
and runtime values:

| Call | Mutex | Update handlers |
|---|---|---|
| `update(fn, origin)` | yes | called with `origin` if the result is `CHANGED` |
| `updateWithoutPropagation(fn)` | yes | no |
| `read(fn)` | yes | no |

The `JsonStateReader<T>` / `JsonStateUpdater<T>` function types connect `T`
to JSON for the file, the REST faces and WS.

### 3.2 `ConfigManager` and `ConfigDelegate<T>`

`lib/ESPRack/include/ConfigManager.h`, `ConfigDelegate.h`.

- **Registration.** `ConfigDelegate` registers a
  `ConfigDescriptor<T>{id, primary path, bufferSize, service, reader,
  updater, validate, formReader}` (`ConfigManager.h:75-102`). The
  Config Manager UI lists every registered entry with load, save, skip and
  snapshot statistics.
- **Load: `ensureLoaded()`.**
  - It calls `loadOrRecover(primary, backup, tmp)`. A valid file gets
    extracted and validated, its secrets decrypted, and the result applied
    through the updater **without propagation** (`:421-469`).
  - Legacy or unmetered files, restores from backup and plaintext secrets
    trigger a "normalize" re-save.
  - If **no** file exists, defaults are seeded and saved. If a file
    **exists but fails** to load, the device runs on defaults in RAM only
    and the file is kept for the next boot, so a transient error can't wipe
    a real config (`:385-418`).
- **Save: `saveIfChanged(origin)`.** Saving is always explicit; the
  `autoSave` parameter is ignored. The call:
  1. serializes with the reader
  2. validates
  3. compares the **plaintext** CRC32 with the last save and skips if
     equal (`:496-502`)
  4. encrypts the secret keys
  5. writes atomically through a tmp file, rotating the old primary into
     `<primary>.snapshot`
  6. fires the observers (`:475-531`)

  Built-in services call it from their own update handler. Each call
  allocates a `bufferSize` document.
- **Snapshots.** `snapshotAll()` and `restoreAllFromSnapshot()` back the
  Config Manager tab's backup and restore actions.
- **Secrets.**
  - When `formReader` is given, `discoverSecretKeysFromForm()` runs the form
    once on a default `T` into a `bufferSize` document and collects the
    keys of every `web::Secret` field (`ConfigManager.h:682-705`).
  - Those **top-level config keys** are stored as `ENC:<hex>`.
  - If the form outgrows `bufferSize`, the probe overflows and keys beyond
    that point are **silently** not encrypted.

### 3.3 File layout

`/config/<id>.json` (each module picks its own name, e.g.
`/config/ntpSettings.json`), `/config/<id>.json.snapshot`, and the work
directories `/config/.tmp` and `/config/.bak` (falling back to `_tmp` and
`_bak`), all on LittleFS.

---

## 4. Web layer: registry and UI manifest

### 4.1 Entry types

`WebManager` (`lib/ESPRack/include/WebManager.h`) keeps one vector of
`IWebFeatureEntry` (`IWebFeatureEntry.h`). Each entry has an `id()` and a
`kind()`, and implements:

- `registerEndpoints(server, sm)`, which mounts its HTTP routes (and WS
  channel) at `WebManager::begin()`;
- `toJson(obj)`, which writes its manifest object;
- `proxyDispatch(...)`, the in-process mothership `rest.proxy`;
- `addTab(...)`, implemented only by compound entries.

| Registration | Entry class | Mounts |
|---|---|---|
| `registerFeature<T>(spec, svc, readers/updaters, buffers)` | `WebFeatureEntry<T>` (`WebFeatureDelegate.h`) | `GET /api/v1/<id>/form`, `GET+POST /api/v1/<id>/state`, plus WS channel `<id>` if `spec.wsPath` is set |
| `registerAction(WebActionSpec)` | `WebActionEntry` | `/api/v1/actions/<id>` (method from the spec, default POST) |
| `registerPage(WebPageSpec)` | `WebPageEntry` | each `WebEndpointMeta` (`handler` or `jsonHandler`) |
| `registerInfoFeature(spec, reader)` | `WebInfoEntry` | GET at `spec.restRead` (unused in-tree) |
| `registerCompoundFeature(spec)` | `WebInfoEntry` | nothing; a tab container such as `system` |
| `addTabToFeature(id, tab)` | — | inserts a tab into a compound entry, sorted by `order` |
| `registerProxyEndpoint(method, path, fn)` | — | `rest.proxy`-only handler for raw `server->on` routes |

Paths are **derived from ids**, not authored:

- `API_V1_PREFIX = "/api/v1"` (`WebFeatureSpec.h:20`).
- Typed features use `derivePaths()` (`WebFeatureDelegate.h:339-343`).
- Actions use `fillActionPath()` (`WebFeatureSpec.h:29-32`).
- A tab's manifest `restPath` is taken in this order: its `formOf` feature's
  form path, else an explicit `restPath`, else the feature's own form path
  (`WebFeatureDelegate.h:182-195`).

The fluent builders (`WebFeature`, `WebMenu`, `WebTab`, `WebAction` in
`WebFeatureBuilder.h`) produce the same structs. Usage is in
[`docs/module-guide.md`](docs/module-guide.md) §5.

### 4.2 Manifest

`GET /api/v1/ui/manifest`, `schemaVersion` 3 (`WebManager.cpp:121-133`):

```
{"schemaVersion":3,"device":{name,version,frameworkVersion},
 "buildFeatures":{<moduleId>:true,...,"project":bool},
 "authenticated":false}                                   ← anonymous tier ends here
 "authenticated":true,"modules":[{id,version}],"features":[<entry>...]}
```

Each entry emits `id`, `kind` (`feature`/`page`/`action`), `title`,
`component`, `auth`, `route` (default `/<id>/*`), `menu{label, icon, order,
auth, hidden, parent, expandTabs, role}`, `rest{read, update, state}` or
`endpoints[]`, `ch` (live channel), `tabs[]` and `actions[]`.

**Serving** (`WebManager.cpp:331-503`):

- `WebManager::begin()` renders the anonymous and authenticated manifests
  **once per boot**, one entry at a time, through a ~4 KB `ManifestBuilder`.
  It writes them to `/config/.manifest.anon.json` and
  `/config/.manifest.authed.json`. The files are only rewritten when the
  content CRC (`/config/.manifest.sig`) changes.
- Requests are served from the file, with an ETag (`<sig>-a` / `<sig>-n`)
  and `304` support. They never return 401; the tier is picked from the
  request's auth.
- If the cache file is missing, the manifest is streamed in chunks per
  request. That path is guarded by `max_alloc < 4096 → 503`.
- **Constraint:** a single entry must serialize to fewer than 2,048 bytes
  (`ManifestBuilder::_itemBuf`, `WebManager.h:76-83`). The largest today,
  `filesystem`, is about 1.86 KB.

**UI side** (`ui/src`):

- `ManifestLoader` fetches the manifest with backoff and gates the app until
  it arrives.
- `AuthenticatedRouting` maps each entry's `route` to `CUSTOM_RENDERERS`
  (`FileSystem`, `Discovery`) or to `DynamicFeature`.
- `LayoutMenu` builds the tree from `menu.parent`, `expandTabs` and `role`
  (see `docs/plans/tree-menu.md`; its `expandTabs` default is stale, the
  code default is `true`).
- `DynamicFeature` renders one panel per tab:
  - live tabs use `DynamicStatus`, subscribed to channel `entry.ch`;
  - other tabs use `DynamicSettings`, fetching from `tab.restPath`.

### 4.3 Forms

A form reader writes `{ "<tabKey>": { "description", "fields": [ {"<key>": value, "o": "<type>;<r|rw>;opt=val;..."} ] } }`.

- The vocabulary for field types and option keys lives in
  `lib/ESPRack/include/web/FieldVocab.h`. It is mirrored by hand in
  `ui/src/components/dynamic-component/utils/fieldParser.ts`; generating one
  from the other is still a TODO.
- There are two authoring APIs:
  - the fluent `web::Form` DSL (`web/Form.h`) — use this;
  - the older `FormBuilder::add*Field` tag API, still used by auto-update,
    cert-manager, config-manager-ui, filesystem, mothership,
    upload-firmware, web-endpoints and ws-diag.
- Parse-back always goes through `FormBuilder::updateValue`.
- The field reference is in the module guide §5.3.

### 4.4 Large responses

- Typed features measure their REST buffer at boot
  (`WEBFEATURE_BUFFER_AUTO`).
- Every JSON response goes through `sendAdaptiveJson` (§8).
- The Endpoints tab, the one response large enough to need it, is streamed
  per registry row with `beginChunkedResponse` (`EndpointsChunker`,
  `modules/web-endpoints/src/WebEndpointsService.cpp:223-340`).
- An earlier `FormStream` built on `AsyncResponseStream` was removed
  (`780b838`), because that primitive buffers the whole body.

---

## 5. WebSocket

`lib/ESPRack/include/WsManager.h`.

### 5.1 One multiplexed socket

- **Socket.** A single `AsyncWebSocket` at `WS_MUX_PATH`, default
  `"/ws/app"` (`:50-52`). The header comment says `/ws`. It's created lazily
  with the first channel.
- **Channels.** A channel is a **feature id**. `WebFeatureEntry` creates a
  `WsManager::Binding<T>` for every feature whose `spec.wsPath` is set.
  `wsPath` is now only a flag (`WebFeatureDelegate.h:112-118`).
- **Live features in-tree:** certManager, ntp, mqtt and telegram, plus
  lightState in the demo.
- **Presence** keeps its own socket at `/ws/presence`
  (`modules/presence`).

Frames:

| Direction | Frame |
|---|---|
| server → client, once on connect | `{"type":"id","id":"ws:<cid>"}` |
| client → server | `{"type":"sub","ch":X}`, `{"type":"unsub","ch":X}` |
| client → server, write | `{"type":"p","ch":X,"origin_id":..,"p":{...}}`; any other `type` that carries an object `p` is treated the same |
| server → client, push and snapshot | `{"type":"p","ch":X,"origin_id":..,"p":<state reader output>}` |

When a client subscribes, the server sends it a snapshot immediately.
There's no replay buffer.

### 5.2 Push pipeline

1. **Trigger.**
   - Implicit: the binding registers an update handler that broadcasts on
     every propagated `CHANGED` update (`:146-156`).
   - Explicit: `WebFeatureEntry::broadcastWs()`.
2. **Gate.** `broadcastCurrentState()` returns early when the channel has
   no subscribers (`:308-317`).
3. **Build.** The frame is serialized into **one shared tx document** sized
   to the largest channel's capacity + 256, created lazily and never freed
   (`:506-568`).
4. **Queue.** It's pushed onto a 16-entry pointer ring. A full ring drops
   the frame silently.
5. **Send.** `processTx` fans out to subscribed clients, skipping any
   client whose AsyncWebSocket queue is at or above
   `WS_BACKPRESSURE_THRESHOLD` (`:589-631`).

`processAllQueues()` (cleanup → rx → tx → ping/evict) runs:
- from `App::loop()`;
- **and** at the end of every WS event on the `async_tcp` task (`:733`).

Ping is every 20 s, and clients silent for 60 s are evicted (`App.cpp:270`).
The client roster is guarded by the recursive `_clientsMx` (`014c4e1`).

The UI (`ui/src/contexts/ws/WsProvider.tsx`, `useWsChannel.ts`):
- reconnects with Sockette, 5 s;
- resubscribes on open, staggered;
- forces a reconnect after 600 s of silence;
- coalesces inbound frames per animation frame;
- throttles outbound writes to one per 500 ms.

### 5.3 Auth

- **Upgrade gate.** The upgrade requires `IS_AUTHENTICATED`, set by
  SecurityModule (`modules/security/include/SecurityModule.h:55-58`). The
  token travels as `?access_token=`.
- **Rejected upgrade.** A rejected upgrade falls through to `onNotFound` and
  gets the SPA shell, not a 401.
- **Per-channel checks** (LIB-FIX-003/011). Each connection's access level
  and operator right are resolved once at `WS_EVT_CONNECT`, through the
  SecurityManager that SecurityModule hands to `WsManager`.
  - `sub` needs at least the channel's (feature's) auth level.
  - `p` additionally needs the operator role and the per-key write level
    (`WriteAuthMap`).
  - Denials are logged and dropped.
  - Identity is per connection: a revoked token keeps its socket until it
    reconnects.
- **Client cap.** `ESPRACK_WS_MAX_CLIENTS` concurrent clients (3 on C3,
  6 elsewhere).

---

## 6. REST API v1 and authorization

### 6.1 Surface

| Surface | Path | Gate |
|---|---|---|
| Typed feature, UI face | `GET /api/v1/<id>/form` | feature `auth` |
| Typed feature, machine face | `GET /api/v1/<id>/state` | feature `auth` |
| Typed feature, write | `POST /api/v1/<id>/state` | feature `auth` **and** `IS_OPERATOR` **and** the per-key write level (`WriteAuthMap`, LIB-FIX-011) |
| Action | `/api/v1/actions/<id>` | action `auth` **and** `IS_OPERATOR` (`WebActionEntry.h:28-34`) |
| Page endpoints | module-chosen, e.g. `/api/v1/fs/*`, `/api/v1/system/*/form`, `/api/v1/discovery/peers` | endpoint `auth`; mutating methods also `IS_OPERATOR` (LIB-FIX-003) |
| Manifest | `GET /api/v1/ui/manifest` | public, two tiers |
| Auth | `POST /api/v1/auth/token`, `GET /api/v1/auth/verify` | public / token |
| Features | `GET /api/v1/features` | public (raw `server->on`) |
| Data series | `GET /api/v1/data[/<series>[/latest]]` | `IS_AUTHENTICATED` |
| Unknown `/api/*` | — | `404` problem+json (`App.cpp:332-336`) |

Contract notes:

- The machine `/state` GET returns the **state reader's** output. For
  features with a separate WS reader, that's the status payload, not the
  POST shape.
- `HttpPostEndpoint` applies the update without propagation, echoes the
  result, and only then calls the update handlers with origin `"http"`
  (`HttpEndpoint.h:137-175`).
- **Prefix-collision rule.** AsyncWebServer treats a handler at `/x` as also
  matching `/x/...`, and the first registered handler wins. So no endpoint
  path may be a prefix of another (`e359983`;
  `modules/system-status/include/SystemStatus.h:13-22`). `DataApi` uses this
  matching on purpose.
- **Errors.** Errors use RFC 9457 `application/problem+json`:
  `sendProblem(req, status, title, detail, retryAfter)`
  (`lib/ESPRack/include/ProblemJson.h`). A 503 carries `retryAfter` and a
  `Retry-After` header. Some legacy 400/401 paths still send bare statuses
  (`HttpEndpoint.h:139,145`, `modules/security/src/AuthenticationService.cpp`).
- **OpenAPI.** `scripts/gen_openapi.py` turns a manifest (from a device or a
  file) into `api/openapi.{json,yaml}`. The committed `api/manifest-dump.json`
  is **stale**: it predates `ch` and still lists `mdns`.

### 6.2 Identities and predicates

`SecurityManager.h:71-89`:

| Predicate | Meaning |
|---|---|
| `NONE_REQUIRED` | no check |
| `IS_AUTHENTICATED` | any valid identity |
| `IS_ADMIN` | authenticated and `user->admin` |
| `IS_OPERATOR` | authenticated and `role != "reader"` |

`WebAuthLevel` (`WebAuth.h`) maps `Public`/`Authenticated`/`Admin` to the
first three.

- **Humans.**
  - JWTs are HS256 with `{username, admin, role, iat, exp}` and a 24 h TTL.
  - `iat`/`exp` are only minted when `TimeSync::clockSane()` holds, so
    tokens minted before a time source made the clock valid never expire.
  - Validation checks that the user still exists and that the `admin` flag
    still matches.
  - Factory users are `admin`/`admin` and `guest`/`guest` (role `user`).
- **Machine tokens.**
  - `tokens.create` mints JWTs with `tid`, `role` (`reader`/`operator`/`admin`)
    and a 10-year `exp`.
  - The registry entry (up to 8 tokens) is authoritative for role and
    revocation.
- **401 vs 403.** A missing or invalid token gets 401; a valid identity
  that fails the predicate gets 403.
- **`NullSecurityManager`** is active until SecurityModule installs, at
  priority 8. It authenticates every request as admin. Modules that install
  earlier capture it (§2.6).
- **Server-enforced levels.**
  - Reads: the server enforces feature, action and endpoint levels.
  - Writes to a typed feature need the **maximum** of:
    - the feature's `auth`;
    - its `writeAuth` floor;
    - the write level of every touched key. That is the tab's `writeAuth`,
      else the tab's `auth` (LIB-FIX-011).
  - Menu `auth` is visibility only.
- **Secrets on the web** (LIB-FIX-004, `SecretFields.h`):
  - Values of `web::Secret` fields are masked for non-admins in GET `/form`
    and `/state`, and always in WS pushes and POST echoes.
  - A posted mask value is dropped.

---

## 7. Secrets, hashing, identity

- **`SecretsVault`** (`lib/ESPRack/include/SecretsVault.h`, `FT_SECRETS_VAULT`):
  - **Format.** `ENC:` + hex(IV‖AES-128-CBC(PKCS7(plain))), with a fresh IV
    on every write.
  - **Key.** The first 16 bytes of `SHA-256(eFuse MAC ‖ "ESP-SES-secrets-v1")`.
    This is obfuscation against casual flash reads: the key is derivable by
    anyone who has the flash dump.
  - **Rejected decrypts.** A wrong-key decrypt that still passes PKCS7 is
    rejected by a control-byte check.
  - **Vault compiled out.** Values that look `ENC:`-shaped are cleared
    rather than passed through as passwords.
  - Wired through ConfigManager (§3.2). Only **top-level** keys are covered.
- **`SecurityHash`**:
  - Stores PBKDF2-HMAC-SHA256 with 2000 iterations, a 16 B salt and a 32 B
    hash, as hex.
  - `verify` compares in constant time.
  - JWT signature comparison does **not**
    (`lib/ESPRack/src/ArduinoJsonJWT.cpp:78`).
- **`DeviceIdentity`** (`DeviceIdentity.h`, header-only, returns `String`).
  `canonical()` = `<project>-<mac12>-<uid8>` and is used as the X.509 CN, the
  mothership device id and the discovery id.

  | Field | Source |
  |---|---|
  | `projectName()`, `version()` | from Builder |
  | `hwRevision()` | `FACTORY_HW_REVISION` |
  | MAC | `macHex12()`, `macColon()` |
  | eFuse UID | `uidHex32()`, `uidHex8()` |
  | flash UID | `flashUidHex()` |
  | chip | `chipModel()`, `flashMB()`, `psramMB()`, `hwSuffix()` (e.g. `n16r8v`), `chipModelFull()` |

- **Firmware tags** (`lib/ESPRack/src/FwTags.cpp`). `BASE_PLATFORM_TAG_<project>`,
  `HW_FLAVOR_TAG_<ESPRACK_HW_FLAVOR_STRING>` and `VERSION_TAG_<version>` are
  embedded in `firmware.bin` for the update server. The flavor string comes
  from the consumer's `compute_flavor.py`.
- **Feature flags** (`Features.h`). Module features are **install-derived**:
  the manifest's `buildFeatures` mirrors the `.install<>()` chain. Only these
  remain:
  - `FT_PROJECT`
  - `FT_SECURITY`
  - `FT_SECRETS_VAULT`
  - `FT_HEAP_MONITOR`
  - `FT_LOOP_PROFILE`
  - `FT_WEBSOCKET`, which nothing references

  The UI gates on `GET /api/v1/features`, a partial map, not on
  `buildFeatures`.

---

## 8. Memory and heap

The binding target is the **no-PSRAM, single-core ESP32-C3**. The constraint
that matters is **contiguous** heap (`max_alloc`), not free heap.

**Response path: `sendAdaptiveJson`** (`AdaptiveJson.h:34-86`)
1. Start the document at the caller's measured hint (minimum 512 B).
2. If the tree overflows, double the size, capped at `max_alloc - 1 KB`
   slack, for up to 5 attempts.
3. Shed `503 heap_pressure` (retryAfter 1) when it can't fit. The UI
   retries a 503 twice.

The document size **is** the heap-pressure gate. That's why buffers are
measured, not guessed (`7d42c22`).

**The "503 zone"** is not a constant in code. It's the band where
`max_alloc` falls to roughly the size of the larger auto-sized form trees
plus slack, about 6–9 KB on C3 (`101be44`).

What pushed `max_alloc` around:

| Change | Effect |
|---|---|
| AsyncTCP stack 16 KB → 6 KB (consumer `platformio.ini`) | raised it from ~9.2 KB to ~18.4 KB (`e359983`) |
| `_peers.reserve(64)` | pinned it at 7,668 B |
| BSS `PeerRow` table | fixed it (`afd1150`) |

Strategies in use:
- Static or BSS task stacks: mothership, telegram, discovery (`06a2cf7`).
- BearSSL `ESP_SSLClient` with static 4 KB in/out buffers, wrapped in
  `PersistentTlsClient`. Telegram still uses `WiFiClientSecure` (mbedTLS).
- The manifest is served from a file.
- One shared WS tx document.
- Measured REST buffers.
- The relaxed HTTP rx/ack timeouts from §2.3.

Diagnostics:
- `[boot] post-<id>` lines after every module's `onBegin()`.
- `HeapMonitor`: a 60-sample drift ring with slope and a per-task stack
  dump, with `FT_HEAP_MONITOR=1`.
- `FT_LOOP_PROFILE`: per-module loop costs.
- The System → Resources tab.
- Every mothership check-in carries `freeHeap`/`maxAlloc`.

Budgets stated in code and commits:

| Chip | Budget |
|---|---|
| C3 | boot `max_alloc` 70–90 KB; steady ~9–18 KB depending on the fixes above; loopTask stack 8 KB; lwIP 16 PCBs; responses ≤ 4 KB target (`DataApi.h:23`); TLS flows need ≥ 7 KB contiguous (`CertManagerService.cpp:56`); soak target ≥ 30 KB |
| S3 with PSRAM | plenty of headroom; soak target ≥ 60 KB |

Plans and their state:
- **`docs/plans/memory-fragmentation-master-plan.md`:** persistent TLS
  (BearSSL), HeapMonitor (off by default) and parts of the String work are
  done. `JsonPool`, the TLS dispatcher and the soak runs are not.
- **`heap-baseline-fix.md`:**
  - Done: Phase 0 (instrumentation), Phase 1 (static stacks), Phase 4
    (`const char*` specs).
  - Not done: Phase 2 (shared ConfigManager document) and Phase 5 (single
    PROGMEM handler).
- **The TLS-OOM comment at `App.cpp:154-161`** refers to a
  `custom_sdkconfig` that doesn't exist in the demo. The active mitigation
  is the BearSSL static buffers.

---

## 9. Messaging providers

- **The core interface is telemetry only.** `ESPRack::IMessagingProvider`
  (`messaging/IMessagingProvider.h`) exposes `providerId()`,
  `connectionState()` and `subscriberCount()`. It **can't send**.
- **Sending is transport-specific.**
  - `ITelegramProvider` / `TelegramSubscription` live in `modules/telegram`.
  - `IMqttProvider` / `MqttSubscription` live in `modules/mqtt`.
- **Getting a provider.** Consumers get them from `app->telegram()` and
  `app->mqtt()` (null when not installed), subscribe once and keep the
  handle.
- **Decoupling is runtime only.** Including those headers ties the build to
  the module libraries.
- **Legacy.** `IMqttDispatcher.h` is the legacy AsyncMqttClient callback
  interface. It lives in the core, so the core's headers depend on a library
  the core doesn't declare.

Usage is in [`docs/module-guide.md`](docs/module-guide.md) §9.

---

## 10. Known issues

The current, prioritized list is in
[`ROADMAP.md` → §0 Status](ROADMAP.md#0-status-2026-10-13). Per-module
caveats are in [`docs/module-catalog.md`](docs/module-catalog.md).
