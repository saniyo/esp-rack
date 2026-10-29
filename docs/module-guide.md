# Writing an ESPRack module

This guide is for anyone adding a feature to ESPRack: a framework module
in `modules/`, or an application module that lives in a consumer repo. Every
statement is checked against the code on `feature/ses-light-support`, and
references point at the source (`path:line` or a symbol name).

The working reference is **[`modules/template/`](../modules/template)**. It's
a small but real hardware module, a UART line monitor, that uses every
surface described below. Start new modules by copying it.

Related docs: [`ARCHITECTURE.md`](../ARCHITECTURE.md) explains how the core
works, [`consumer-guide.md`](consumer-guide.md) shows how a project installs
modules, and [`module-catalog.md`](module-catalog.md) lists the built-in
modules.

---

## 0. Quick start

1. Copy `modules/template/` to `modules/<your-dir>/` (framework module) or
   into the consumer's own `src/` (application module, like
   `esp-rack-light-demo/src/LightControlModule.h`).
2. Rename everything:
   - library name in `library.json`
   - `TemplateModule`/`TemplateService`/`TemplateState`
   - the descriptor id `"template"`, the feature id, the action id
     `template.*` and `TEMPLATE_*`/`FACTORY_TEMPLATE_*` macros
   - the config file `/config/template.json`
3. Consumer side: add the library name to `lib_deps` (the demo lists every
   module explicitly and uses `lib_compat_mode = strict`, see
   `esp-rack-light-demo/platformio.ini`). Then `#include <YourModule.h>` and
   `.install<YourModule>(ctorArgs...)` in the Builder chain.
4. Build, open the UI and check the boot log for
   `[ESPRack] installed '<id>' ...` and `[boot] post-<id>: free=… max_alloc=…`.
   That line shows how much heap your `onBegin()` took (`lib/ESPRack/src/App.cpp:250-260`).

---

## 1. Where code goes: core vs module

| Layer | Location | What belongs there |
|---|---|---|
| Core | `lib/ESPRack/` (library `ESPRack`) | Hardware-independent machinery only: the Module/Builder/App lifecycle, persistence, web/WS/REST plumbing, security primitives, **interfaces and arbitration** (e.g. provider interfaces such as `messaging/IMessagingProvider.h`). |
| Module | `modules/<dir>/` (one PlatformIO library each) | Anything that talks to hardware, a network service or a protocol: WiFi, MQTT, Telegram, OTA, and future GPS/PZEM drivers. Installed with `.install<>()` and pulled in through `lib_deps`. |
| Consumer | consumer repo `src/` | The device's own application module(s) plus `main.cpp` with the Builder chain. |

**Rule:** hardware drivers never go into `lib/ESPRack`. If several modules
need a shared concept (a time source, a location provider), the *interface*
goes into the core and each implementation is a module.

---

## 2. Directory layout and naming

```
modules/<dir>/
  library.json            # PlatformIO library manifest
  include/<Name>Module.h  # thin ESPRack::Module subclass (header-only)
  include/<Name>Service.h # state struct + service class
  src/<Name>Service.cpp   # implementation
```

`library.json` (copy `modules/template/library.json`):

- `"name"` is what consumers put in `lib_deps`, by convention `<Name>Module`
  (e.g. `NTPModule`, `MqttModule`). The directory name and library name don't
  have to match: `modules/wireguard` is `WireGuardModule`.
- `"dependencies": [{"name": "ESPRack"}]`, plus the external libraries the
  module needs.
  - The in-tree modules don't declare their third-party libraries today
    (`AsyncMqttClient`, the Telegram bot, `WireGuard-ESPRack`, and core's
    `ESP_SSLClient`), so consumers list them in `lib_deps` by hand.
  - New modules should declare them.
- `"frameworks": ["arduino"]`, `"platforms": ["espressif32"]`.
  `lib_compat_mode = strict` in the consumer rejects libraries that don't
  declare these.
- `"build": {"srcDir": "src", "includeDir": "include"}`.

**Includes are flat.** Every library's `include/` dir goes on the include
path, so you write `#include <Module.h>`, `#include <ConfigDelegate.h>` and
`#include <NTPModule.h>`. The `<modules/wifi/WiFiModule.h>` form shown in
`lib/ESPRack/include/ESPRack.h` and the old README does **not** match how the
demo builds (`esp-rack-light-demo/src/main.cpp:18-40` uses `<WiFiModule.h>`).

Naming conventions in the tree:

| Item | Convention | Examples |
|---|---|---|
| Module class | `<Name>Module` | `NTPModule`, `MqttModule`, `LightControlModule` |
| Service class | `<Name>Service` or `<Name>SettingsService` | `TelegramService`, `NTPSettingsService` |
| State / settings struct | `<Name>Settings` / `<Name>State` | `NTPSettings`, `LightState` |
| Descriptor id | camelCase, unique | `"ntp"`, `"lightControl"`, `"configManagerUi"` |
| Feature id (UI) | camelCase, often the descriptor id | `"ntp"`, `"lightState"` |
| Action id | dotted `<scope>.<verb>` | `light.reset`, `system.restart`, `wifi.scan` |
| Config file | `/config/<name>.json` (module's choice) | `/config/ntpSettings.json`, `/config/lightState.json`, `/config/template.json` |
| Factory defaults | `FACTORY_<MODULE>_<KEY>` with `#ifndef` fallback | `FACTORY_NTP_SERVER` (`modules/ntp/include/NTPSettingsService.h:43-54`) |

---

## 3. The Module contract

`lib/ESPRack/include/Module.h:110-132`:

```cpp
class Module {
 public:
  virtual ~Module() = default;
  virtual void describe(ModuleDescriptor& d) = 0;   // phase A
  virtual void onInstall(ModuleContext& ctx) = 0;   // phase B
  virtual void onBegin() {}                         // phase C
  virtual void onLoop() {}                          // phase D
  virtual void onShutdown() {}                      // phase E
};
```

### 3.1 When each hook runs

| Hook | Called from | Environment at that moment |
|---|---|---|
| ctor | your `.install<M>(args...)`, which constructs the module in place (`Builder.h:40-46`) | Nothing exists yet. Store the args, nothing else. |
| `describe(d)` | `Builder::build()` step 1 (`Builder.cpp:67-74`), again in `App::begin()` (`App.cpp:194-200`, `250-254`) and on every profiled loop | Must be **pure and idempotent**. It's called many times. |
| `onInstall(ctx)` | `Builder::build()` step 5, once, in sorted order (`Builder.cpp:161-178`) | **Filesystem not mounted, WiFi not initialized.** Construct services, `registerFeature/Action/Page`, `addTabToFeature`. Don't load config here. |
| `onBegin()` | `App::begin()` step 4, in install order (`App.cpp:250-260`) | LittleFS mounted (step 1), `ConfigManager::begin()` done (step 2), `WiFi.mode(WIFI_AP_STA)` done (step 3). **HTTP endpoints and WS channels are not mounted yet.** That happens after all `onBegin`s in `WebManager::begin()` (step 5, `App.cpp:265`), and `server.begin()` is the last step (`App.cpp:408`). Load config, open peripherals, subscribe to providers here. |
| `onLoop()` | `App::loop()`, every iteration, in install order, followed by `WsManager::processAllQueues()` (`App.cpp:461-465`) | Cooperative and single-threaded on `loopTask`. **Never block** (no `delay()`, no unbounded reads). |
| `onShutdown()` | `ESPRack::restartDevice(reason)`, which every built-in reboot path uses, or the `esp_restart()` shutdown hook (`App.cpp`, LIB-FIX-006) | Runs **once**, in reverse install order, on the loop task when possible, with a budget of `ESPRACK_SHUTDOWN_BUDGET_MS` (500 ms) per module, then the chip resets. Persist counters and put outputs (relays, SSRs) into a safe state here. `ESPRack::restartReason()` says why: for example, on `kRestartFactoryReset` the `/config` wipe happens *after* your hook, so don't bother saving. Never block on the network. **Task context:** normally the loop task. In the 3 s fallback (the loop didn't take the hand-off), or from the IDF safety net (an `esp_restart()` outside `restartDevice`, reason `"idf"`), it runs on **another task**. Don't touch loop-owned state there without the service lock. After the reboot, `ESPRack::lastRestartReason()` returns the reason. |

### 3.2 `ModuleDescriptor` (`Module.h:81-106`)

| Field | Default | Meaning |
|---|---|---|
| `id` | `nullptr` | Unique dependency key. Also used as the manifest `buildFeatures[id]` key and in `modules[]` (`App.cpp:194-200`). Static literal. |
| `version` | `"0.0.0"` | Informational; surfaced in the manifest/System tab. |
| `requires_` | `{}` | Hard dependencies (ids). Builder orders you after them. |
| `priority` | `50` | Tie-breaker 0..100, lower = earlier. |
| `enabled` | `true` | `false` = Builder skips the install (build-flag opt-out). |

### 3.3 `ModuleContext` (`Module.h:63-73`)

`server`, `cfgMgr`, `web`, `ws`, `security`, `presence`, `app`,
`deviceName`, `deviceVersion`. `security` and `presence` are refreshed
after each install (`Builder.cpp:175-176`). Every other provider is
reached through `ctx.app`: `app->mqtt()`, `app->telegram()`, `app->tls()`,
`app->cert()`, `app->mothership()`, `app->wireguard()` (`App.h:83-134`).
They are `nullptr` when the module providing them isn't installed. Always
null-check.

### 3.4 Ordering, dependencies and priorities

`Builder::build()` runs Kahn's topological sort. Among the modules that are
ready, it picks the lowest `(priority, id)` (`Builder.cpp:97-133`).
Consequences:

- **`requires_` is the only ordering guarantee.** `priority` only breaks ties
  among modules whose dependencies are already satisfied.
- **Optional providers:** fetch `app->mqtt()`/`app->telegram()` in
  `onBegin()`. By then every `onInstall` has run, so every provider that
  exists is set. The demo reads them in `onInstall` and relies on
  priority 60 > 40 (`esp-rack-light-demo/src/LightControlModule.h:33-46`).
  That works today but is fragile.
- **Validation is not fail-fast**, despite the comment in `Builder.h:85-90`:
  - A missing dependency, duplicate id or cycle only logs `log_e`.
  - A missing dependency leaves the module's in-degree above zero, so it gets
    reported as a "dependency cycle" and the whole install order falls back
    to **insertion order** (`Builder.cpp:88-95`, `135-142`).
  - Watch the boot log.
- A module with `enabled = false` is skipped, but it still satisfies other
  modules' `requires_` (`Builder.cpp:164-167`).
- `.core()`, `.networking()` and `.full()` are **empty stubs** today
  (`Builder.cpp:28-38`). Install every module explicitly.

Priorities used in-tree, from each `*Module.h::describe()`:

| Priority | Modules |
|---|---|
| 5 | `features` |
| 6 | `uiDyn` |
| 7 | `presence` |
| 8 | `security` (the comment at `Module.h:60-62` still says 5) |
| 10 | `wifi`, `ap` |
| 12 | `cert-manager` |
| 14 | `wireguard` |
| 20 | `filesystem` |
| 24 | `discovery` |
| 30 | `ntp` |
| 35 | `mothership` |
| 40 | `autoUpdate`, `mqtt`, `ota`, `telegram`, `uploadFirmware` |
| 60 | app modules (`lightControl`, `template`) |
| 80 | `systemStatus`, `wsDiag` |
| 85 | `configManagerUi`, `webEndpoints` |
| 90 | `restart`, `factoryReset` |

Details per module are in [`module-catalog.md`](module-catalog.md).

---

## 4. State and persistence

### 4.1 One state struct + `StatefulService<T>`

A module keeps **one** class `T` that holds both persisted settings and
runtime values. Its service derives from `StatefulService<T>`
(`lib/ESPRack/include/StatefulService.h`), which gives you:

| API | Use |
|---|---|
| `read(fn)` / `read(json, reader)` | Read under the recursive mutex (`:108-118`). |
| `update(fn, origin)` / `update(json, updater, origin)` | Mutate under the mutex. If the result is `CHANGED`, it **calls every update handler** with `origin` (`:74-99`). |
| `updateWithoutPropagation(...)` | Mutate under the mutex, **no handlers** (`:84-106`). Use for high-rate runtime values. |
| `addUpdateHandler(cb, allowRemove)` | Subscribe to propagated updates. `cb(const String& origin)` (`:55-62`). |
| `_state` | The protected instance (`:127`). Prefer `read()`/`update*()` once more than one task can touch it. |

`origin` is a free-form string, e.g. `"boot"`, `"tick"`, `"http"` (REST
POST, `HttpEndpoint.h`), `"rest.proxy"`, or `"ws:<cid>"`.

- The UI is meant to use it to filter its own echoes.
- That doesn't work reliably today. The UI sends back the origin of the
  last frame it *received* (`DynamicStatus.tsx:198`), so a WS write can
  arrive labelled `"tick"`.
- Don't make security decisions on `origin`.

**Threads.**
- `onLoop()` runs on `loopTask`.
- REST handlers, actions and page endpoints run on the AsyncTCP task.
- WS writes are applied in `WsManager::processAllQueues()`, which runs from
  **both** `App::loop()` and the WS event handler (`WsManager.h:733`).

So anything a web path can reach must go through the service mutex, and
hardware must be driven from `onLoop()` only. Web paths just set a flag
(`TemplateService::_applyPending`).

### 4.2 `ConfigDelegate<T>`: the file on flash

Construct it in the service's init list (`ConfigDelegate.h:19-30`):

```cpp
_cfg(cfgMgr,
     "template",                 // id — Config Manager tab label, unique
     "/config/template.json",    // primary file
     2048,                       // bufferSize (see below)
     this,                       // the StatefulService<T>
     TemplateState::readConfig,  // T -> JSON written to the file
     TemplateState::update,      // JSON -> T (file load AND every write path)
     false,                      // autoSave: ignored; saving is always explicit
     nullptr,                    // optional validator(JsonObjectConst)
     TemplateState::buildForm)   // formReader: enables secret auto-discovery
```

Semantics, verified in `lib/ESPRack/include/ConfigManager.h`:

- **Load in `onBegin()`** with `_cfg.ensureLoaded()`. On the first boot (no
  primary and no snapshot file) the updater runs on an empty object, so your
  C++ defaults stay, and the defaults get written (`:385-418`). If a file
  exists but fails to verify, defaults run **in RAM only** and the file is
  kept for the next boot (`:392-413`). Load uses
  `updateWithoutPropagation`, so no handlers fire.
- **Saving is explicit.** Every built-in settings service registers
  `addUpdateHandler([this](const String& o){ _cfg.saveIfChanged(o); }, false)`.
  See `modules/ntp/src/NTPSettingsService.cpp:28-32` and the template's
  constructor. `saveIfChanged` serializes with `readConfig`, compares a
  **plaintext CRC32** and skips the flash write when nothing changed
  (`:496-502`). Otherwise it does an atomic write, rotating the previous file
  into `<primary>.snapshot` (`:504-531`).
- `ConfigDelegate::update(fn, origin)` mutates **and** persists in one call
  (`ConfigDelegate.h:111-120`). `subscribe(cb)` fires only after a
  successful write (`ConfigManager.h:120`).
- Keep **runtime values out of `readConfig`**. Otherwise every tick looks
  "changed".
- **With a `formReader`, the form runs inside the `ConfigDelegate`
  constructor**: registration renders the whole form once to find the secret
  keys. Make `_cfg` the **last member** of the service (and last in the
  initializer list). A member declared after it isn't constructed yet when
  the form reads it: a status row reading an `std::atomic<const char*>`
  declared after `_cfg` crashed a CAN port on boot (`LoadProhibited` at
  `0xa5a5a5a5`).
- `saveIfChanged` allocates a `bufferSize` document each call. Don't route
  10 Hz runtime updates through propagated `update()`. Use
  `updateWithoutPropagation()` and push WS yourself (§6).

### 4.3 Parse-back: the updater

The UI's Save sends a **flat object with only the changed keys**
(`ui/src/components/dynamic-component/DynamicSettings.tsx:276-284`).
Machine clients POST to `/api/v1/<id>/state` with the same shape. Your
updater must therefore accept partial dicts.

- Use `FormBuilder::updateValue(src, "key", field)` for each key; it's a
  no-op when the key is absent.
- Accept the `{settings:{...}}` envelope as well, like `NTPSettings::update`
  does (`NTPSettingsService.h:152-165`).
- Never fill absent keys with factory defaults: a Save that edits one field
  would reset all the others. Defaults belong in the state's member
  initialisers. The first boot and `resetToDefaults()` start from `T{}`.
  OTA and `JsonUtils::readIP` used to get this wrong; LIB-FIX-001 fixed both.

The UI's Save path (since LIB-FIX-001):

1. A postable tab reads `/api/v1/<id>/form`.
2. On Save it POSTs the changed keys to `/api/v1/<id>/state` (the same path
   with `/form` replaced by `/state`).
3. It then refetches the form.

Machine clients use the same POST.

**Field types matter** (`lib/ESPRack/include/FormBuilder.h`):

- `bool` accepts only a real JSON bool.
- `uint8_t`, `uint16_t`, `int`, `float` and `double` coerce from numbers
  **and numeric strings**, with clamping or rounding.
- `long`, `unsigned long`, `long long` and `unsigned long long` follow the same
  rules (`web/IntCoerce.h`): integers clamped to the type, doubles
  rounded and clamped, decimal strings parsed and clamped, negatives clamped to 0
  for unsigned. A non-numeric string is ignored, with a `log_d` naming the key. On
  ESP32 (esp-14.2.0) these are `int32_t`, `uint32_t`, `int64_t` and `uint64_t`.
- **Any other type** (`String`, enums, ...) uses a strict template that
  requires `is<T>()`. Clamp after parsing,
because REST and WS callers bypass the UI's min/max (template `update()`,
or `LightState::updFs` at `esp-rack-light-demo/src/LightStateService.cpp:94-96`).

### 4.4 Secrets (encryption at rest)

Mark a field as `web::Secret("pwd", s.password.c_str())` in the form and
pass the form reader as the **last** `ConfigDelegate` argument. At
registration, `discoverSecretKeysFromForm` runs your form on a
default-constructed `T` and collects every key whose option string starts
with `secret;`. Those keys are written as `ENC:<hex>` on flash and
decrypted on load (`ConfigManager.h:682-705`, `:441-447`, `:504-507`).

Constraints:

- The form key must equal the **top-level config key** (`"pwd"` in both
  `readConfig` and `web::Secret`, as in mqtt/ap/wifi/ota).
- **Discovery probe.** It grows from 2 KB to 32 KB until the whole form
  fits. If it still doesn't fit, ConfigManager logs `log_e` and encrypts
  *every* top-level string key of that config. A secret is never silently
  stored in plaintext (LIB-FIX-004).
- **Masking on the web** (automatic, `SecretFields.h`):
  - GET `/form` and `/state` return `********`, or `""` when nothing is
    set, to non-admins. Admins see real values.
  - WS pushes and POST echoes are always masked.
  - A posted value equal to the mask is dropped, so it never overwrites the
    stored secret.
- **Still don't emit secrets from a state/WS reader.** The masking only
  knows the keys that appear as `web::Secret` in the form.

---

## 5. UI: features, tabs and forms

### 5.1 Register a typed feature

From `onInstall`, via `ctx.web`. Use the fluent builder
(`lib/ESPRack/include/WebFeatureBuilder.h`, umbrella include `<web/Web.h>`):

```cpp
_feature = web->registerFeature<TemplateState>(
    WebFeature("template")
        .title("UART monitor")
        .version("0.1.0")                         // service rev (optional)
        .component(WebComponent::DynamicSettings)
        .authenticated()                          // feature-wide auth
        .menu(WebMenu("UART monitor").icon("Cable").order(60))
        .ws("/ws/template")                       // marks the feature LIVE
        .tab(WebTab("status", "Status").icon("Monitor").live())
        .tab(WebTab("settings", "Settings").icon("Tune").postable().adminOnly())
        .build(),
    this,
    TemplateState::buildForm, TemplateState::update,   // form face
    TemplateState::readState, TemplateState::update,   // state / WS face
    WEBFEATURE_BUFFER_AUTO,                            // REST buffer: measured at boot
    1024);                                             // WS frame buffer
```

This mounts the following, with paths **derived from the id**
(`WebFeatureDelegate.h:60-121`, `WebFeatureSpec.h:20`):

| Path | Served by | Gate |
|---|---|---|
| `GET /api/v1/<id>/form` | form reader (`buildForm`), the UI schema | feature `auth` |
| `GET /api/v1/<id>/state` | state reader (`readState`); with the 2-reader overloads, the **form reader** otherwise | feature `auth` |
| `POST /api/v1/<id>/state` | updater, then echo | feature `auth` **and** operator role, i.e. not a `reader` token (`:98-106`) |
| WS channel `<id>` on the single socket | state reader + WS updater | any authenticated socket; see the WS warning in §6 |

`.rest(...)`, `.readOnly(...)` and `WebAction::rest(...)` are **deprecated
no-ops** (`WebFeatureBuilder.h:194-212`, `142-144`). Don't author paths.

`registerFeature` overloads (`WebManager.h:118-161`):
- `(spec, svc, reader, updater, buf)` uses one reader for everything.
- `(spec, svc, restR, restU, wsR, wsU, buf)` separates the form and state
  readers, with one buffer size.
- The 8-argument form takes separate REST and WS buffer sizes.

Prefer separate readers: the form reader builds UI schema, and the state
reader emits flat values.

`WEBFEATURE_BUFFER_AUTO` (= 0, `HttpEndpoint.h:29`) runs your form reader
once at mount time into a 24 KB probe and sizes the buffer to the measured
usage + 512 B (`WebFeatureDelegate.h:310-334`). Use it. Forms that are bigger
than ~8 KB don't fit the no-PSRAM C3's `max_alloc` and need streaming.

### 5.2 Menu and tabs

`WebMenu` (`WebFeatureBuilder.h:73-104`):

| Method | Effect |
|---|---|
| `.icon(name)` | MUI icon name. `WebIcon::` holds typo-safe constants; any MUI name works. |
| `.order(n)` | Sort key. The System shell uses 900 (`App.cpp:126`). |
| `.nestUnder(id)` | Nest under another feature/group in the tree menu. The composition can move a module's items (`Builder::under`, `WebManager::placeMenu`; consumer guide §9.5). A module never places itself; it lists what it uses in `ModuleDescriptor::uses_` (e.g. its factory port), and an unplaced item used by one module nests under it. |
| `.expandTabs(bool)` | Tabs become tree leaves (default **on**). |
| `.role()` | Marks the device's main-role feature for the discovery menu. The UI opens it first unless a feature is marked `.landing()`. |
| `.landing()` | The page the UI opens first, and where the brand in the sidebar leads (one per firmware). Without it the role feature, else the first menu item. |
| `.hidden()` | Keeps the feature off the menu. |
| `.auth(...)`, `.adminOnly()`, ... | Menu **visibility** only. |

`WebTab` (`:107-130`):

| Method | Effect |
|---|---|
| `key` | Must match a `web::Form` name emitted by the form reader. |
| `.live()` | The tab subscribes to the feature's WS channel. |
| `.postable()` | The tab shows Save. It **should** POST to `rest.update` (`/state`), but it currently POSTs to the tab's `restPath` (`/form`) and gets a 404; see §4.3. |
| `.order(n)` | Sorting, used when tabs are contributed by several modules. |
| `.icon(name)` | Icon for the tree leaf. |
| `.auth(...)` | Tab **visibility** in the UI, and since LIB-FIX-011 also the server-side **write level** of the keys rendered in the tab, unless `.writeAuth()` overrides it. Reads use the feature's auth level. |
| `.writeAuth(level)` / `.adminWrite()` | Explicit server-side write level for this tab's keys. |

The write rule (`lib/ESPRack/include/WriteAuth.h`):

- A POST `/state` or a WS `p` frame needs the **maximum** of:
  - the feature `auth`;
  - `WebFeature::writeAuth()`, the write floor for the whole feature;
  - the level of every key it touches, taken from the tab(s) whose form
    renders that key.
- Too low gets `403 problem+json` over REST; the frame is dropped over WS.
- The keys of wifi, ap, mqtt and ntp are admin-write: users can see those
  pages but can't change them.

To contribute a tab to an existing feature, e.g. the framework's `system`
shell (`App.cpp:120-130`):

```cpp
WebTabSpec tab; tab.key = "ota"; tab.title = "OTA"; tab.formOf = "ota"; ...
web->addTabToFeature("system", tab);     // WebManager.h:276-285
```

`formOf = "<typedFeatureId>"` resolves to that feature's derived form path
at manifest time (`WebFeatureSpec.h:72-77`). Use it instead of a literal
path.

Other registrations (`WebManager.h:164-193`):

- `registerInfoFeature(spec, reader)` is a stateless read-only feature at
  `spec.restRead`.
- `registerCompoundFeature(spec)` is a menu shell whose tabs other modules
  fill.
- `registerPage(WebPageSpec)` is for bespoke React renderers (only
  `FileSystem` and `Discovery` exist: `ui/src/AuthenticatedRouting.tsx:36-39`)
  and for raw endpoints (§8).

### 5.3 Form DSL reference

A form reader writes one `web::Form` per tab key
(`lib/ESPRack/include/web/Form.h`). Every field has a key, a value and an
access flag: `rw` by default, `.readOnly()` for `r`.

**Universal options** (all field types, `Form.h:41-58`):

| Option | Effect |
|---|---|
| `.label(t)` | Label text. |
| `.icon(muiName)` | Field icon. |
| `.color(c)` | MUI color. |
| `.card(groupId)`, `.cardTitle(t)`, `.cardDeletable()` | Group fields into cards. |
| `.hideAvatar()` | Hides the field avatar. |
| `.refetchForm()` | Refetches the form after a change. |
| `.showIf("otherKey", "value")` | Shows the field only when another field has that value. |
| `.loadingIf("field:prefix")` | Shows a loading state while a field has that prefix. |

| Builder | Value type | Type-specific options |
|---|---|---|
| `web::Text(key, const char*)` | string | `.placeholder() .unit() .colorMap()`. `.tile()`: an overview tile (below) |
| `web::Secret(key, const char*)` | string, **encrypted at rest** (§4.4) | `.placeholder() .colorMap()` |
| `web::Textarea(key, String)` | string | `.placeholder() .colorMap()` |
| `web::Ip(key, const char*)` | string | `.placeholder() .unit() .colorMap()` |
| `web::TimeZones(key, label)` | string (tz label) | — |
| `web::Checkbox / Switch / Button(key, bool)` | bool | `.placeholder() .colorMap()` |
| `web::Number(key, double)` | **emitted as a string** at the format's precision (`:192`) | `.min() .max()` (**default 0..100, always emitted, so set them**), `.format("0.00")` (decimals), `.unit() .placeholder() .colorMap()`. No `step`. `.tile()`: an overview tile (below) |
| `web::Slider(key, double)` | number | `.min() .max() .step()` (defaults 0/100/1) |
| `web::Dropdown / Radio(key, int)` | int | `.option("Label", value)` repeated |
| `web::Dropdown(key, const char*)` | string | `.option("Label", "value")` repeated; no `,` or `:` in either. For instance ids: `app->hw()->instanceOptions<I>(dropdown, selected)` lists the instances implementing `I` and keeps a stale `selected` visible |
| `web::DateTime(key, int32_t seconds)` | epoch seconds | `.dateOnly() .timeOnly() .dateTime()`. `.timeOfDay()`: the value is **seconds since local midnight** (0..86399) shown as HH:MM with no timezone conversion; clamp it in the updater |
| `web::Trend(key)` | read-only chart | `.mode("lineChart"/"barChart"/"pieChart")`, `.xAxis("timestamp")`, `.line(key, color, type)`, `.legend() .tooltip() .maxPoints(n)`. Also: `.xFormat("epoch"/"time"/"date"/"datetime"/"dom"/"month")`, where x is epoch seconds (≥ 1e11 = ms) labelled in the **device** timezone; without it x shows raw. `.category()`: x is the `xAxis` key and every push replaces the rows. `.stack(a, b[, c])` stacks bars. `.refLine(key, color, label)` draws a horizontal line at a sibling value; `.vrefLine(key, color, label)` a vertical one at the x it names (the equal category, else the nearest x; hidden when there is none), labelled "label value" on top. `.yMin(v)` / `.yMax(v)` fix an end of the y axis; `.yAuto(pad)` lets the free ends follow the visible data with a margin of pad × span instead of starting at 0. History mode (History module): `.history(seriesId)` adds a period switch (Live, 24 h, 7 d, 30 d, 1 y, a day; `.periods(csv)`, `.period(p)`, `.liveLabel(s)`), `.unit(u)`, and `.histCol(col, sign)` after a `.line()` names its history column (`key`, `key[i]`, `key[min]`) and sign. A category chart shows each element over the period; a time series reads the period at a resolution it needs (about 340 rows). `.liveSince(key)`: a state key with the epoch the Live data starts at. `.zero()`: 0 always on the y axis, with a zero line. `.height(px)`: the plot height (default 400). `.overlay(back, front)`: bars of two series over each other in the same category place, `back` behind (a row without `back` shows `front` alone). `.tipKey(key)`: a row's text the tooltip adds under the values. `.signedArea(pos, neg)` after a `.line()` (lineChart): the area under that line, `pos` above 0 and `neg` below (a gradient split at 0), e.g. a day's load `.line("p", "#555", "monotone").signedArea("#1e88e5", "#eb6834")`; a colour may be a theme palette key (`info`, `warning`, …). |
| `web::Progress(key, double)` | read-only bar | `.min() .max()`, `.maxKey(k)` (live max from a sibling), `.markerKey(k)` (marker at a sibling value, e.g. a plan), `.unit() .format("0.0")`, `.thresholds("0:error,60:warning,95:success")` (% of max → colour) |
| `web::Flow(key)` | read-only power flow, the whole row | `.node(id, title, icon)` and `.edge(from, to)`, each followed by `.valueKey(k) .unit(u) .subKey(k)` (they apply to the last node or edge); `.dirKey(k)` on an edge. See *Power flow* below |
| `web::Table(key)` | rows (initially `[]`) | `.col(key, label, type[, fmt])`: type `text`/`number`/`boolean`, or in the device timezone `date` (fmt `month` → "Oct 2026"), `time` (fmt `tod`: seconds of day), `datetime`, `duration` (fmt `s`/`ms`/`min` → "2h 05m"); `.mode("append"/"upsert"/"replace")`, `.maxRows(n)`, `.layout("grid"/"cards")`, `.primary() .secondary() .avatar() .badge()`, `.rowFill() .rowFillFallback() .rowNav()`; seed rows with `form.rowsOf(key)` |
| `web::Files(key, restBase)` | path | `.initial(path) .uploadBase(path)` |
| `web::Upload(key, url)` | — | `.accept(mime)` |
| `web::Action(key[, label])` | button | `.ref("action.id")` (manifest action), or `.url() .method() .confirm() .withFields("param=field,...")`. `.withClientTime("epoch", "ms")` appends the browser's clock on click: `.ref("system.setTime").withClientTime("epoch","ms")` sets the clock from this browser |
| `web::Message(key, text)` | static alert, always read-only | `.level("info"/"warning"/"error"/"success")`. `.levelKey(k)` takes the severity from a sibling's live value when it is one of those four |

**Overview tiles.** `.tile()` on a `web::Number` or `web::Text` shows it as a
tile: the label as a caption (with the icon), the value big with its unit
(a number at its `.format()` decimals), and `.colorMap()` colouring the value
(a palette key such as `success`, or a CSS colour). A tile is read-only.
`.subKey(k)` adds a line under the value from a sibling key (a line break in
it starts a second line). Lay tiles out with `web::Cols(n)`: four in a row look
good, and `Cols(5)` fits five small ones; `.span(k)` works as usual.

```cpp
form.add(web::Cols(4));
form.add(web::Number("soc", s.soc).format("0").unit("%").label("Battery")
             .icon("BatteryFull").tile().subKey("soc_sub").colorMap("100:success,default:info"));
form.add(web::Text("mode", s.mode).label("Mode").tile());
```

**Power flow.** `web::Flow(key)` draws nodes in a row (a column when the
widget is narrower than 420 px) in the order given, with an edge between
neighbours (`edge(a, b)` or `edge(b, a)`; an edge between non-neighbours is
not drawn). A node shows its title, its icon, its `valueKey` value big with
the unit and the `subKey` text under it (a line break starts a second line).
An edge shows its value with the unit over a moving dashed line, the `subKey`
text under it, and the direction from `dirKey`: `+1` = flow `to -> from`
(e.g. charge, blue), `-1` = `from -> to` (discharge, orange), `0` = idle
(grey, no arrow). Colours come from the theme (info / warning / disabled).
The manifest carries only the keys; every value is a sibling value (below),
so send the signed power as text through `subKey` if it should show. Up to
6 nodes and 5 edges; ids, titles, icons, keys and units must not contain
`, ; : | =` (refused and logged).

```cpp
form.add(web::Flow("pf").label("Power flow")
    .node("bat", "Battery", "BatteryFull").valueKey("soc").unit("%").subKey("bat_sub")
    .node("inv", "Inverter", "SolarPower").valueKey("inv_w").unit("W")
    .edge("bat", "inv").valueKey("p_kw").unit("kW").dirKey("p_dir").subKey("p_sub"));
```

Rules worth knowing:

- The value key is emitted **before** `"o"` because the live renderer reads
  the first key as the value (`Form.h:84-94`). Use the builders and never
  hand-roll fields.
- A form reader also runs at boot on a **default-constructed** state, once
  for the buffer probe and once for the secret probe. Keep it a pure
  function of `s`: no service members, no hardware I/O. The demo mirrors
  provider data into state for exactly this reason
  (`LightStateService.cpp:336-340`).
- The wire vocabulary lives in `web/FieldVocab.h` and is mirrored by
  `ui/src/components/dynamic-component/utils/fieldParser.ts`. A new field
  type needs both sides.
- Colors baked into the form (`.color()`) only change on refetch. WS ticks
  update values only (`NTPSettingsService.h:128-132`).
- **Sibling values** (`Progress.maxKey/markerKey`, `Trend.refLine/vrefLine`,
  `Number/Text.subKey`, every `Flow` key) name
  another state key. On a live tab that can be any key of the WS reader
  (it doesn't have to be a field). On a REST-only tab it must be a field of
  the same form.
- **Device time.** The manifest carries `device.tz = {iana, posix}` from
  `TimeSync`, and `xFormat` labels are formatted in that zone, not the
  browser's. Without an IANA label the POSIX standard offset is used, shown
  as "UTC±N". A timezone change re-bakes the manifest, and a page picks it
  up on reload.
- Editable (`rw`) fields on a **live** tab are written over WS on blur
  (`DynamicStatus.tsx:198`, a `{"type":"p", ch, p:{key:val}}` frame).
  They reach the **WS updater**, not the REST one.

---

## 6. Live status over WebSocket

- **One socket per browser tab**, `WS_MUX_PATH` (default `/ws/app`,
  `WsManager.h:50-52`), multiplexed by channel. The **channel name is the
  feature id**. `spec.wsPath` is only an on/off flag now
  (`WebFeatureDelegate.h:112-118`, `174`); any non-empty string works.
- Envelope (`WsManager.h:11-17`):
  - client → server: `sub`, `unsub`, `p` (write)
  - server → client: `id`, `p` (push and snapshot), `hb`
  - The server sends a snapshot to a client when it subscribes. There is no
    replay buffer.
  - Every 5 s (`WS_HEARTBEAT_MS`) each client gets `{"type":"hb"}`. After 15 s
    without any frame the UI reconnects and subscribes again.
  - A client whose queue stays full for 20 s (`WS_STUCK_MS`) is stuck. Its
    connection is aborted. System → Sockets shows the count as
    `stuck_evictions`, and `slow_s` per client.
- **What gets pushed** is your state reader's output: a flat object.
  - **Trend:** emit an array under the trend's key with one point
    `{timestamp, <lineKey>: value}` per push (template `readState`;
    `LightStateService.cpp:123-131`).
  - **Table:** emit an array of new rows under the table key; the frontend
    appends, upserts or replaces according to `.mode()`
    (`LightStateService.cpp:144-188`).
- **When it gets pushed:**
  - **Automatically after every propagated `update()`.** The feature's
    `WsManager::Binding` registers its own update handler (`WsManager.h:146-156`).
    Don't call `broadcastWs()` from your update handler too: NTP does
    (`NTPSettingsService.cpp:31`) and sends every change twice.
  - **Manually** with `_feature->broadcastWs(origin)` for runtime values
    you changed with `updateWithoutPropagation()`.
- **Cadence and throttling are the module's job.** The framework has no
  rate limiter. Gate periodic work on `_feature->hasSubscribers()`
  (`WebFeatureDelegate.h:296-299`) and a millis() period from settings
  (template `pushLive()`, `NTPSettingsService::loop()` at `NTPSettingsService.cpp:85-96`).
  The only server-side protection is per-client back-pressure, which drops
  frames for slow clients (`WS_BACKPRESSURE_THRESHOLD`, `WsManager.h:61-63`,
  `604-612`). `broadcastCurrentState` also skips serialization when a
  channel has no subscribers (`:308-317`).
- `broadcastWs()` from `onBegin()` does nothing, because the WS binding is
  created later in `WebManager::begin()` (`WebFeatureDelegate.h:115`).
  The `broadcastWs("boot")` calls in NTP and the demo are harmless no-ops.
- Frame size: the `wsBufferSize` you pass (the 8-argument overload). WS
  frames are built in one shared document sized to the largest channel's
  capacity (`WsManager.h:236-238`).

- **Client cap.** `ESPRACK_WS_MAX_CLIENTS` concurrent clients per device
  (default 3 on C3, 6 elsewhere; override with `-D`). Above the cap
  `cleanupClients()` closes the oldest one (LIB-FIX-002).
- **Access control per channel** (LIB-FIX-003/011):
  - Each connection's level and role are resolved at connect.
  - `sub` needs at least the feature's auth level.
  - A `p` write additionally needs the operator role (not a `reader` token)
    and the per-key write level (see §5.2).
  - Denials are logged and dropped.
  - The identity is fixed for the connection's lifetime.

---

## 7. Actions (buttons)

```cpp
web->registerAction(
    WebAction("template.resetCounters", "Reset counters")
        .icon(WebIcon::Refresh).color("secondary")
        .authenticated()                    // default is Admin (WebFeatureSpec.h:102)
        .confirm("Reset the RX counters?")  // confirm dialog; omit to fire immediately
        .success("Counters reset")          // toast text
        .handler([this](AsyncWebServerRequest* r) {
          updateWithoutPropagation([](TemplateState& s) { s.bytesRx = 0; /*...*/ return StateUpdateResult::CHANGED; });
          r->send(200, "application/json", "{\"ok\":true}");
        }));
```

- Mounted at **`POST /api/v1/actions/<id>`** (`WebFeatureSpec.h:22-32`).
- Gated by the action's auth level **plus the operator floor**, so `reader`
  tokens get 403 (`WebActionEntry.h:31-33`).
- Put the button in a form with `web::Action("a_reset").ref("template.resetCounters")`.
  Framework actions can be referenced the same way: `system.restart`,
  `system.factoryReset` (`LightStateService.cpp:235-239`).
- The handler runs on the AsyncTCP task: use the service mutex, and never
  drive hardware directly.
- Optional `.internalHandler([](JsonVariant in, JsonVariant out, int& status){...})`
  makes the action callable in-process by the mothership `rest.proxy` (no
  HTTP, no JWT; `WebFeatureSpec.h:107-120`).

---

## 8. Custom REST endpoints

For anything that isn't a form/state pair, wrap raw handlers in a
`WebPageSpec`. The registry mounts them with auth wrapping and publishes them
in the manifest, the Endpoints tab and OpenAPI (`WebPageEntry.h:32-73`):

```cpp
WebPageSpec page;
page.id    = "templateLines";            // no menu.label => not in the side nav
page.title = "UART monitor: recent lines";
page.auth  = WebAuthLevel::Authenticated;
WebEndpointMeta e;
e.method  = "GET";
e.path    = TEMPLATE_LINES_PATH;         // "/api/v1/template/lines" — a literal
e.auth    = WebAuthLevel::Authenticated;
e.handler = [this](AsyncWebServerRequest* r) { serveLines(r); };
page.endpoints.push_back(std::move(e));
web->registerPage(std::move(page));
```

- **Handler shapes:**
  - `handler(req)` for GET or simple POST.
  - `jsonHandler(req, JsonVariant&)` for a JSON body (POST/PUT/PATCH/DELETE).
  - `internalHandler(in, out, status)` for the mothership proxy.
  - Don't set both `handler` and `jsonHandler` (`WebFeatureSpec.h:129-160`).
- **Responses:**
  - JSON bodies go through `sendAdaptiveJson(req, fill, startBytes)`
    (`AdaptiveJson.h:34`). It sizes the document to the payload and sheds a
    retryable `503 heap_pressure` instead of failing an allocation. `fill`
    may run more than once, so keep it free of side effects.
  - Errors go through `sendProblem(req, status, title, detail)`
    (`ProblemJson.h:28`, RFC 9457 `application/problem+json`).
- **Paths:**
  - Keep paths under `/api/v1/<your-id>/...`.
  - AsyncWebServer treats a handler at `/x` as also matching `/x/...`.
    Never mount a path that is a prefix of another module's path (see
    "prefix-collision" in `ARCHITECTURE.md`).
  - Unknown `/api/*` paths return a problem+json 404 (`App.cpp:332-336`).
- **ArduinoJson 6 string semantics:** assigning a `const char*` *links* the
  pointer, while `char*` and `String` are *copied*. Responses are serialized
  after your lock is released (chunked, per TCP ack), so copy anything that
  points into mutable state (template `serveLines()` / `readState()`).
- **Mutating page endpoints get the operator floor automatically.** That
  covers any non-GET method and any `jsonHandler`: a `reader` token gets 403
  (LIB-FIX-003).
- **Time series on flash** (periods, rollups, retention, CSV): use the
  History module (`app->history()->series(def)`, `history/IHistory.h`,
  module-catalog `history`), not a store of your own.
- **Short in-RAM histories:** register a paged series with
  `DataApi::instance().registerSeries({id, title, unit}, fill)`.
  - It's served at `GET /api/v1/data/<id>?limit&cursor` (newest first,
    `limit` ≤ 100) and readable by `reader` tokens (`DataApi.h:11-60`).
  - Reference: the `light.events` ring in
    `esp-rack-light-demo/src/LightStateService.cpp:558-592`.
  - It's a good fit for energy or position history.

---

## 9. Messaging: Telegram and MQTT

Providers are optional. Fetch them in `onBegin()`:

```cpp
#include <ITelegramProvider.h>   // pulls TelegramModule's headers: add it to lib_deps
#include <IMqttProvider.h>       // likewise MqttModule

ITelegramProvider* tg = app->telegram();   // nullptr when not installed
IMqttProvider*     mq = app->mqtt();
```

**Telegram** (`modules/telegram/include/ITelegramProvider.h`,
`TelegramSubscription.h`, `TelegramTypes.h:38-61`):

```cpp
TelegramSubscriptionConfig cfg;
cfg.tagPrefix            = "UART: ";  // prepended to every message (Markdown-safe!)
cfg.maxMessagesPerMinute = 2;         // per-subscription quota (default 60)
// cfg.defaultChatId / cfg.defaultTopicId — empty = bot defaults
_tg = tg->subscribe("template", cfg);         // keep the handle as a member
...
if (_tg.active()) _tg.send("line rate high");  // active() = valid && unmuted && bot enabled
```

`sendTo(recipient, text, opts)` overrides the destination for one message.
`send` returns `InvalidMessagingSendId` when the quota is used up, the queue
is full or the subscription is muted.

Commands: the bot answers `/<cmd> [args]` from the chats in
Telegram → Settings → "Command chats" (an id, a `-group` or `group:topic`;
empty = the default chat). The handler runs on the loop task and returns the
reply; `adminOnly` commands need the sender in "Admin user ids". Polling
runs only while a command is registered; enabling the bot needs no reboot.

```cpp
_tg.onCommand("status", "Current state", [this](const TelegramInbound& in) { return statusText(); });
_tg.onCommand("resetlockout", "Clear the lockout", [this](const TelegramInbound&) { return reset(); }, true);
```

**MQTT** (`modules/mqtt/include/IMqttProvider.h`, `MqttSubscription.h`,
`MqttTypes.h:20-53`):

```cpp
MqttSubscriptionConfig cfg;
cfg.defaultTopicPrefix    = "home/uart/";  // joined verbatim — keep the trailing '/'
cfg.defaultQos            = 0;
cfg.defaultRetain         = true;          // for "current state" topics
cfg.maxPublishesPerMinute = 4;             // 0 = unlimited
_mq = mq->subscribe("template", cfg);
if (_mq.active()) _mq.publish("rate", "12.5");          // -> home/uart/rate
_mq.onMessage("cmd/#", [this](const String& topic, const String& payload) { /*...*/ });
```

Device availability belongs to the provider, not to a
subscription. With MQTT → Settings → Availability on, the device publishes a
retained `online` after every connect, and the broker publishes the retained
`offline` will when the device drops off. Reference the topic with
`mq->availabilityTopic()` ("" when off), e.g. in a discovery payload. Don't
publish there yourself.

Live demo of both: `LightStateService::begin()` and `loop()`
(`esp-rack-light-demo/src/LightStateService.cpp:487-508`, `594-646`).

Including `ITelegramProvider.h`/`IMqttProvider.h` makes the provider module a
**compile-time dependency**: its library must be in the consumer's
`lib_deps` even when it isn't installed. The core's
`messaging/IMessagingProvider.h` only exposes telemetry (`providerId`,
`connectionState`, `subscriberCount`), so it can't send.

---

## 10. Hardware modules

The SES rule: hardware drivers are modules, and the module never decides
which peripheral or pins to use.

1. **Inject the peripheral through the constructor.**
   `explicit TemplateModule(HardwareSerial& port = Serial1)`. The consumer
   writes `.install<TemplateModule>(Serial2)`. Do the same for `TwoWire&`
   and `SPIClass&`.
2. **Pins, baud and addresses are settings, not constants.**
   - Factory defaults come from `#ifndef FACTORY_<MOD>_RX_PIN` macros that
     the consumer overrides in `factory_settings.ini`.
   - They're persisted through `ConfigDelegate` and editable in the UI.
   - `-1` means "the UART's board default" (`HardwareSerial::begin(baud,
     SERIAL_8N1, rx, tx)` keeps the default pin for `-1`).
3. **Touch hardware only from `onLoop()`/`onBegin()`.** An update handler
   (AsyncTCP or WS context) sets an atomic flag, and `loop()` re-opens the
   port (`TemplateService::applyPort()`).
4. **Never block.**
   - Drain a bounded number of bytes per tick (`kBudget = 256` in
     `drainPort()`).
   - Run Modbus request/response as a state machine with a millis()
     timeout. No `delay()` and no blocking `readBytes()` with long timeouts.
   - If something truly blocks, give it its own FreeRTOS task and hand
     results over through the service mutex.
5. **Fixed memory.** Line buffers and rings are fixed arrays in state (the
   template's `ring[8][64]`). Avoid growing `String`s and `std::vector`s in
   hot paths. Discovery's `PeerRow` table is the in-tree example
   (commit `afd1150`).
6. **UART budget per chip** (Arduino `Serial1`/`Serial2`):
   - ESP32-C3 has 2 UARTs. UART0 is the console, with or without USB CDC
     on boot (the IDF console stays there), so the registry refuses it.
   - ESP32-C6 has 2 HP UARTs plus 1 LP UART.
   - ESP32-S3 has 3 UARTs: GPS on `Serial1` and PZEM on `Serial2` fit next
     to the console.
   - The module mustn't assume any of this. The consumer passes the port.
7. **Shared concepts belong in the core as interfaces.** A module that
   provides time never calls `settimeofday()`. It registers with core
   `TimeSync` and offers samples from `onLoop()`:

   ```cpp
   #include <time/TimeSync.h>
   // onInstall:
   ESPRack::TimeSync::instance().registerSource(ESPRack::TimeSource::Gps);
   // onLoop, when a fix arrives; stamp it when it was TAKEN, with
   // TimeSync::monoMs() (64-bit; millis() wraps after 49.7 days):
   ESPRack::TimeSync::instance().submit({ESPRack::TimeSource::Gps, utcMs,
                                         fixMonoMs, /*accuracyMs*/ 100, 0});
   ```

   The arbiter decides whether the sample may step the clock (NTP > GPS >
   external RTC > operator > soft-reset hold). Location goes through
   `App::location()` (`geo/ILocationProvider.h`): a GPS module calls
   `submitGps()`, it doesn't depend on the location module. Code that needs
   a trustworthy clock checks `ESPRack::TimeSync::clockSane()` or
   `isValid()`, and can `subscribe()` (in `onBegin`) to clock events.
8. **BLE peripherals** (a balancer, a sensor) never touch the BLE stack
   themselves. `BleModule` owns it and publishes `ESPRack::IBleProvider`
   (`ble/IBleProvider.h`, `app->ble()`). A driver registers a central link
   in `onBegin` and keeps the returned `IBleLink*`:

   ```cpp
   ESPRack::BleLinkSpec spec;
   spec.owner = "balancer";
   spec.setNamePrefixes("GW-,EK-");    // or spec.setAddress(mac, type)
   spec.setService("ffe0");
   spec.setNotifyChar("ffe1");         // the write char defaults to it
   spec.onNotify = [this](const uint8_t* d, size_t n) { feed(d, n); };     // BLE host task
   spec.onState  = [this](ESPRack::BleLinkState s) { if (s == ESPRack::BleLinkState::Connected) hello(); };
   _link = app->ble()->registerLink(spec, &why);   // null + why when refused
   ```

   The module finds the peer, connects, subscribes and reconnects with
   backoff. Callbacks must copy and return. Add `requires_ = {"ble"}` and
   `h2zero/NimBLE-Arduino` to the consumer's `lib_deps`.
9. **Never include another module's header** unless your `library.json`
   lists that module in `dependencies`. PlatformIO's library finder (chain
   mode) follows every `#include` without evaluating `#if` or
   `__has_include`, so such an include drags the other module (and its own
   dependencies) into every build that uses yours. Contracts between modules
   are interfaces in the core (`lib/ESPRack/include`, e.g. `ICertProvider`,
   `IMothershipProvider`, `IWireguardProvider`), late-bound through `App`;
   restart goes through the core's `ESPRack::restartDevice()`.
   `scripts/check_module_deps.py` checks this.
10. **Boot loops and the loop watchdog** (`system/BootGuard.h`,
   `system/LoopWdt.h`):
   - Read `ESPRack::BootGuard::info().safeMode` in `onBegin()`. In safe mode,
     leave outputs off; the crash run that caused it is in `info()`.
   - With `LoopWdt` armed by the consumer, one `loop()` pass longer than 5 s
     resets the chip. Never block. Bracket one-off long work (erasing NVS, a
     big file) with `LoopWdt::pause()` / `resume()`.
11. **Reboot only through `ESPRack::restartDevice(reason)`**, never
   `ESP.restart()`, and make `onShutdown()` leave the hardware safe: outputs
   off, counters persisted, within 500 ms. A stray `esp_restart()` still
   runs the hooks through the shutdown handler, but on whatever task called
   it.

### 10.1 External HW modules (instances)

A driver that may be installed more than once (two energy meters, three
relay boards) is an **external HW module**. It derives from
`ESPRack::ExtHwModule` (`hw/ExtHwModule.h`) and the consumer installs named
instances:

```cpp
.installExtHw<TemplateExtHwModule>("relay1", /*outPin=*/4, /*buttonPin=*/9)
.installExtHw<TemplateExtHwModule>("relay2", /*outPin=*/5)
```

Start from `modules/template-ext-hw/`. Its pieces:

1. **The id names everything.** The module id is `instanceId()` (Builder
   uses it whatever `describe()` says). The feature id and WS channel are
   `featureId()`. The config is `configId()` / `configPath()` (`hw-<id>`,
   `/config/hw-<id>.json`). Actions are `"<id>.<verb>"`. Registrations keep
   pointers, so build those strings into member buffers.
2. **`describeHw(d)`** lists what the instance uses now, from its settings:
   `d.add(HwKind::Gpio, outPin, "out", kHwOutput)`. Use −1 for "not set" and
   `kHwOptional` for a resource the instance works without. Set
   `d.disabled` when the settings switch the instance off. The boot pass
   calls it before `onBegin`, so load the config there
   (`_cfg.ensureLoaded()`). Bus pins are shared claims with the bus number
   and a role: `d.add(HwKind::Gpio, sda, "sda", kHwShared, bus)`, next to
   `HwKind::I2cBus` (shared) and the device's `HwKind::I2cAddr`. A device
   on an I2C bus instance claims only its address (item 16).
3. **Load verbatim, check web writes.** Give `ConfigDelegate` the plain
   updater, so a saved clash shows up as `Conflict` instead of losing the
   file. Make it fill in the factory pins when the payload is empty (no
   file yet, or a reset to defaults, which starts from `T{}`). Give `registerFeature` an updater that applies to a copy, calls
   `hw()->validate(*this, proposed)` and, when that isn't ok, calls
   `ESPRack::setUpdateError(c.reason)` and returns
   `StateUpdateResult::ERROR`. The UI then shows the reason, e.g. "GPIO35
   reserved: octal PSRAM/flash".
4. **After a change, `hw()->update(*this)`.** Call it in the update handler,
   then switch pins in `loop()`.
5. **`hwState()` other than `Ok` means: hands off.** Don't drive the pins
   of an instance in `Conflict`.
   - **Release, then say so**. Hardware an instance gives up
     (new pins, Disabled, Conflict, NotConfigured) stays claimed as
     "releasing". Other instances get "GPIO18 being released by meter".
   - Once the loop has let it go (pins back to input, `Serial2.end()`),
     call `hwReleased()` (or `hw()->released(*this)`).
   - An instance that never says so keeps that hardware blocked. System →
     Hardware shows "releasing N". After 10 s the state reads `· warn` and the
     note "releasing N for 14 s, no released()". The claim stays.
   - Only hardware the instance set up waits for `released()`. An instance
     that never was `Ok` (a conflict since boot) gives its claims up at once.
   - **Report what the hardware does**. `Ok` only means the
     resources are free. Use `hwNote("no answer from the meter",
     HwNoteLevel::Fault)` (or `hw()->setRuntimeNote(...)`) and
     `hwNote(nullptr)` once it works. System → Hardware shows
     `Ok · fault` and the note.
6. **Interfaces.** Other code finds the instance with
   `app->hw()->find<IThing>("relay1")`. Implement
   `queryInterface()` with `ESPRack::ifaceIs(ifaceId, IThing::kIfaceId)`.
   The interface carries a versioned id (`"thing.v1"`). Shared interfaces go
   in a header that the drivers implementing them share, not in the core.
7. **`restoreFactoryHw()`.** Put the ctor pins back and save. System →
   Hardware's "Restore factory pins" calls it, then restarts.
8. **Framework modules** that own pins don't become instances. They call
   `ctx.app->hw()->reserveGpio(pin, "reason")` in `onInstall`, as the
   filesystem module does for the SD card. For a UART, an RMT/LEDC channel
   or an I2C address use `ctx.app->hw()->reserve(HwKind::Uart, 2, "reason")`.
   Consumers use `Builder::reserve(...)` / `reserveUart(port, reason)`.
9. **Strings the form uses exist before the `ConfigDelegate`**.
   `ConfigDelegate`'s constructor registers the config, and that runs the
   form reader once (the secret-key probe). Any instance string the form
   reader reads (an action id, a title) must already be filled in by then.
   Either declare those buffers before the `ConfigDelegate` member and fill
   them in its initializer, or keep the form reader a pure function of the
   state, as `template-ext-hw` does.
10. **UARTs: map to `Serial0/1/2`, never `new HardwareSerial`.** The Arduino
    core keeps a pointer to the first serial object started on each UART for
    good. A `HardwareSerial` you create and delete leaves it dangling. Take a
    `HardwareSerial&`, or map the configured port number to
    `Serial0`/`Serial1`/`Serial2` and only call `begin()`/`end()` on it.
11. **Persistence and power**.
    - Counters and checkpoints go to an `NvsRecord<T>` (with a
      `CheckpointPolicy`), not to a JSON config; state that must survive a
      soft reset goes to `ESPRACK_RTC_SCRATCH`.
    - Use `ESPRack::SupplyState` for supply states; a module that senses
      the supply reports it with `PowerGuard::setSupply()`.
    - Before any flash write of your own, ask `PowerGuard::writesAllowed()`.
12. **Owner and menu**.
    - The composition root says which module uses an instance:
      `.installExtHw<MeterModule>("meter", Serial1).owner("sesLight")`. The
      owner is a module id, and its menu entry has the same id.
    - Build the instance's menu entry only with `hwMenu(label)`. The UI places
      it from the manifest's `extHw[]`: in an "External Hardware" group under
      the owner, or under System without one. Never `nestUnder()` it; its
      tabs stay in the page header.
    - The page says "Used by <owner>". Hide manual controls the owner drives
      (`hwOwned()`), and add your own note where it helps.
    - `hwTitle()` and `interfaceIds()` fill `extHw[]`.
13. **Static form schema**, opt-in: `WebFeature(...).staticSchema()`.
    The UI then caches `GET <form>?schema=1` (values nulled, ETag/304) and
    takes the values from `/state`. Only for a form whose fields never depend
    on the state:
    - every field's key is in the state reader under the same name (table
      rows too), and unique across the feature's tabs: the state is flat
      (WiFi's status tab shows the live connection as `sta_*`);
    - no `if (state...)` around `f.add(...)`: use `showIf`;
    - no options built from runtime data: show such data as table rows, as
      the WiFi scan does.

    Two more cases:
    - **A bespoke page tab** (`addTabToFeature`, a raw handler) opts in with
      `WebTab(...).staticSchema(statePath)`. Its handler answers `?schema=1`
      with `sendFormSchema(...)`, and `statePath` serves the flat values.
      Example: System → External Hardware.
    - **A live feature's state reader** also builds its WS frames, which have
      a fixed buffer. `ESPRack::stateFace()` (`StateFace.h`) tells a REST read
      (`Rest`: GET /state, which a static-schema page reads all its values
      from) from a WS frame (`WsSnapshot`, `WsPush`). Send the full lists
      only in `Rest`, and only what changed in WS frames. Example: MQTT's seen
      topics.
14. **Interrupt timing between instances** (`hw/IrqTimed.h`).
    - A driver that decodes its input from interrupt timing (an edge ISR
      measuring pulse widths: OpenTherm, IR, 433 MHz) implements
      `IIrqTimed` (`"irqTimed.v1"`). `irqQuiet()` is true while nothing is
      being received and nothing can start before its next `onLoop`.
    - It keeps quiet moments coming, e.g. a pause between frames.
    - A driver that masks interrupts for more than ~100 us (a bit-banged
      DHT22 read) calls `ESPRack::irqQuiet(hw(), &busy)` on loopTask right
      before it masks. When that is false, it tries again on a later pass,
      never forces the read, and shows how long it has been waiting.
15. **CAN ports** (`hw/ICanPort.h`).
    - Only the CAN interface is an external HW instance (`can-twai`: TX/RX
      pins, controller index; a chip with two controllers is two
      instances). It implements `ICanPort` (`"canPort.v1"`): `begin(bitrate)`,
      `end()`, `write(frame)`, `read(frame, timeoutMs)`, `stats()` (bus-off
      count and frame counters).
    - The software on the bus is a plain module, `install<M>(portId)`. In
      `onBegin` it takes `app->hw()->find<ICanPort>(portId)`, calls
      `claim(myId)` (one user per port; a second claim fails and the port
      page shows who holds it) and wraps the port in its library's
      transport. The port id is a setting; a change ends, releases and
      claims again without a reboot.
    - The application reaches a plain module's interface with
      `app->find<I>(moduleId)` (`Module::queryInterface`, same ids as
      external HW). Look it up once and keep the pointer.
16. **I2C bus** (`hw/II2cBus.h`).
    - The bus is an external HW instance of its own (`i2c-bus`: controller,
      SDA/SCL). It claims `HwKind::I2cBus` and both pins exclusively and
      implements `II2cBus` (`"i2cBus.v1"`): `write`, `writeAt` (register
      bytes + data, no STOP between), `read`, `writeRead` (repeated start),
      `probe`, `stats()`. Each call is one transaction, from any task,
      serialised by the bus lock: it waits at most `kLockWaitMs` for the
      lock (`Busy`: normal under load, try again), then the transfer has
      its own timeout. It returns an `I2cResult`; `i2cResultName()` names
      it for logs. Every method is safe in every bus state; a bus that
      isn't started answers `NotReady`.
    - A device claims only its address, in `describeHw` (the bus id is a
      setting): `i2cDeviceClaim(hw(), busId, 0x68, "rtc", d)`. Two devices
      conflict only on the same address on the same controller. Without a
      bus the device is `NotConfigured` ("i2c bus not set"). It keeps
      `hw()->find<II2cBus>(busId)` from `onBegin` and retries on `NotReady`.
      When the bus-id setting changes it calls `hw()->update(*this)` and
      looks the bus up again, as a CAN user does with its port id.
    - A register or memory access is one transaction: read with
      `writeRead(addr, reg, ...)`, write with `writeAt(addr, reg, ..., data, n)`
      (a FRAM's 2-byte address + up to `kMaxXfer - 2` bytes). Several
      transactions with nothing from another task in between (a status
      read-modify-write, a write and its verify read) go under
      `I2cBusGuard g(bus, waitMs)` when `g.ok()`. The lock is recursive per
      task.
    - The bus comes first in install order. Give its module a `priority`
      below its device drivers' and install it before them in the Builder
      chain: Builder sorts by `requires_`, then priority, and falls back to
      the order of the install calls when a dependency is missing (§3.4). A
      driver that opens `Wire` itself (item 2) can't share a controller with
      a bus instance: the second one is `Conflict`.
    - The bus page lists its devices with
      `hw()->forEachClaim(HwKind::I2cAddr, fn)`, keeping the rows for which
      `i2cAddrOn(r, controller())` holds (bus -1 counts as I2C0).

`install<M>()` on an `ExtHwModule` and `installExtHw<M>()` on a plain module
both fail to compile with a message saying which one to use. An invalid id
(only `[a-z0-9_-]`, 1–23 chars) or a duplicate id is logged. That instance
isn't installed, and System → Hardware lists it as "Not installed".

---

## 11. Memory rules of thumb

- **The no-PSRAM C3 is the binding target.** Steady `max_alloc` is about
  9–10 KB (`WebFeatureDelegate.h:322-328`). Any single allocation near that
  size (a big form buffer, a `DynamicJsonDocument`) can trigger
  `503 heap_pressure` or fail.
- **Use measured buffers:** `WEBFEATURE_BUFFER_AUTO` for forms, a small WS
  buffer (1–2 KB) and `sendAdaptiveJson` for custom responses.
- **Watch `[boot] post-<id>`.** It logs free/max_alloc after your
  `onBegin()`. With `FT_LOOP_PROFILE=1` in `features.ini`, `App::loop`
  logs per-module time, heap and stack deltas every 60 s
  (`App.cpp:420-459`).
- **Keep tasks and stacks lean.** Every FreeRTOS task's stack is a
  contiguous heap block. See the AsyncTCP stack note in the demo's
  `platformio.ini`.

---

## 12. Checklist before you ship a module

- [ ] `library.json` name/deps/frameworks/platforms set, and the library
      is added to the consumer's `lib_deps`
- [ ] `describe()`: unique id, version, `requires_` for hard deps,
      sensible priority
- [ ] `onInstall`: construct and register only; config loaded in `onBegin`
- [ ] Update handler calls `_cfg.saveIfChanged(origin)`, sets flags and
      does **no** extra `broadcastWs`
- [ ] Runtime values use `updateWithoutPropagation` + gated `broadcastWs`
- [ ] Updater accepts partial flat dicts, uses coercing field types and
      clamps
- [ ] Number fields have explicit `min/max`
- [ ] No secrets in the state/WS reader; secrets are `web::Secret` and
      `formReader` is passed with enough `bufferSize`
- [ ] Hardware is touched only from `onBegin/onLoop`, reads are bounded,
      there's no `delay()`, and no pin is hardcoded
- [ ] `onShutdown()` puts outputs in a safe state within 500 ms; reboots
      go through `ESPRack::restartDevice()`
- [ ] Settings that only admins may change use `adminOnly()`/`adminWrite()`
      tabs or `WebFeature::adminWrite()` (server-enforced)
- [ ] Mutating endpoints are actions or WebPageSpec endpoints (both get
      the operator floor)
- [ ] Builds for the demo envs (C3 at least, plus your target chip)
