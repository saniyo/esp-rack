# ESPRack module catalog

What each `modules/*` library does, how to install it, its public API, its
web/REST/WS surface, what it persists, what it costs, and its known caveats.

This is an audit snapshot of `feature/ses-light-support`, dated 2026-10-13:

- **Method.** Every entry was written by reading the code; nothing was
  checked at runtime. References are `path:line` relative to the repo root.
- **Byte costs.** Cost figures are what the code or its comments state.
  "not measured" means exactly that.
- **Where the issues go.** The cross-module findings at the end feed the
  prioritized list in [`ROADMAP.md` §0](../ROADMAP.md#0-status-2026-10-13).
- **New modules.** See [`module-guide.md`](module-guide.md) and start from
  `modules/template/`.

> **Status update (2026-10-13, after this audit).** Several findings below
> were fixed by LIB-FIX-001…011 on `feature/ses-light-support`:
>
> - mqtt no longer echoes its password (004);
> - ota partial updates no longer reset the port and password (001);
> - the filesystem purge only removes `*.partial` (007);
> - presence and uiDyn are authenticated (005);
> - ntp, telegram and cert-manager no longer push WS twice (009);
> - `tokens.create` builds valid JSON (008);
> - restart paths run `onShutdown()` (006);
> - WS and page-endpoint authorization (003);
> - tab-level write auth is enforced on the server (011);
> - the root bundle and versions are fixed (010).
>
> SES-LIB-002 then moved clock ownership into the core: `TimeSync` decides
> which time may set the clock, owns the TZ, and replaces the hard-coded
> clock checks in security, cert-manager and the TLS client (see `ntp`).
>
> The current list is [`ROADMAP.md` §0.4](../ROADMAP.md#04-known-issues).
> Entries below are kept as audited.

## Conventions used by all modules

- **Lifecycle.** A module fills `describe()` (id, version, `requires_`, `priority`) and implements `onInstall`. It can also override `onBegin`, `onLoop` and `onShutdown` (`lib/ESPRack/include/Module.h:110-132`). `Builder::build()` runs Kahn's topological sort and always picks the lowest `(priority, id)` that is ready (`lib/ESPRack/src/Builder.cpp:97-133`). `App::begin()` calls `onBegin` in the same order (`lib/ESPRack/src/App.cpp:250-260`).
- **Presets.** `.core()`, `.networking()` and `.full()` are documented in `lib/ESPRack/include/Builder.h:59-67`, but they are **empty TODO stubs** (`lib/ESPRack/src/Builder.cpp:28-38`). No module is installed by any preset today, so every module must be installed explicitly with `.install<X>()`. None of the module constructors take arguments.
- **Derived REST paths.**
  - A typed feature gets `GET /api/v1/<featureId>/form` and `GET`+`POST /api/v1/<featureId>/state`. The POST also requires `IS_OPERATOR` (`lib/ESPRack/include/WebFeatureDelegate.h:73-106`).
  - An action mounts at `POST /api/v1/actions/<actionId>` (`lib/ESPRack/include/WebFeatureSpec.h:22-32`).
- **WebSocket.** A feature with a non-null `wsPath` is "live". It becomes channel `<featureId>` on the single multiplexed socket `WS_MUX_PATH = "/ws/app"` (`lib/ESPRack/include/WsManager.h:51`, `lib/ESPRack/include/WebFeatureDelegate.h:112-118`). The module's own `*_WS_PATH` string only acts as the "this feature is live" flag.
- **Auth.** The HTTP gate is the feature's `spec.auth` (`lib/ESPRack/include/WebFeatureDelegate.h:79`). A tab-level `auth` is only written into the manifest (`:198`) and is not enforced on the server.
- **Install order when every module is installed** (derived from the algorithm, not verified at runtime): `features` (5), `uiDyn` (6), `presence` (7), `security` (8), `ap` (10), `wifi` (10), `cert-manager` (12), `wireguard` (14), `filesystem` (20), `discovery` (24), `ntp` (30), `mothership` (35), `autoUpdate`, `mqtt`, `ota`, `telegram`, `uploadFirmware` (40), `systemStatus`, `wsDiag` (80), `configManagerUi`, `webEndpoints` (85), `factoryReset`, `restart` (90).
- **Persistence.** No module uses NVS or `Preferences`. All persistence is JSON under `/config` on LittleFS through `ConfigDelegate`. Fields emitted as Secret are auto-discovered and encrypted at rest by `SecretsVault`.

## Summary table

| module dir | class | priority | depends on | one-line purpose |
|---|---|---|---|---|
| ap | `APModule` | 10 | none | Soft-AP plus captive-portal DNS for provisioning and fallback |
| auto-update | `AutoUpdateModule` | 40 | wifi | Periodic HTTP pull-OTA from an update server |
| cert-manager | `CertManagerModule` | 12 | wifi | Device PKI: ECDSA key, CSR, enroll, rotate, recover; mTLS identity |
| config-manager-ui | `ConfigManagerUiModule` | 85 | none | System → Config Manager tab with snapshot/restore actions |
| discovery | `DiscoveryModule` | 24 | wifi | UDP multicast discovery of sibling ESPRack devices on the LAN |
| factory-reset | `FactoryResetModule` | 90 | none (uses restart, undeclared) | Delete `/config/*` and reboot |
| features | `FeaturesModule` | 5 | none | `GET /api/v1/features` install map |
| filesystem | `FileSystemModule` | 20 | none | File-manager REST API over flash (and optional SD) |
| mothership | `MothershipModule` | 35 | wifi, cert-manager | mTLS check-in client and server-command dispatcher |
| mqtt | `MqttModule` | 40 | wifi | MQTT client plus `IMqttProvider` subscription API |
| ntp | `NTPModule` | 30 | wifi | SNTP and timezone |
| ota | `OTAModule` | 40 | wifi | ArduinoOTA (espota) network-upload listener |
| presence | `PresenceModule` | 7 | none | Registry of active SPA tabs, plus `/ws/presence` |
| restart | `RestartModule` | 90 | none | `system.restart` action and `restartNow()` helper |
| security | `SecurityModule` | 8 | presence | Users, JWT, API tokens; the real `SecurityManager` |
| system-status | `SystemStatusModule` | 80 | none | System → Identity and Resources tabs, `/api/v1/system/health` |
| telegram | `TelegramModule` | 40 | wifi | Telegram bot sender plus `ITelegramProvider` |
| ui-dyn | `UiDynModule` | 6 | none | Legacy `/api/v1/ui/dynamic` control registry (always empty) |
| upload-firmware | `UploadFirmwareModule` | 40 | none (uses restart, undeclared) | Firmware upload from the browser |
| web-endpoints | `WebEndpointsModule` | 85 | none | System → Endpoints registry tab |
| wifi | `WiFiModule` | 10 | none | STA with two credential slots, failover and scan |
| wireguard | `WireGuardModule` | 14 | wifi, cert-manager | On-demand WireGuard tunnel to the mothership |
| ws-diag | `WsDiagModule` | 80 | presence | System → Clients tab (presence table) |
| template | `TemplateModule` | 60 | none | Skeleton to copy for new modules: UART line monitor (see module-guide) |

---

### `ap` — APModule
- **Purpose:** Runs the soft-AP used for first-boot provisioning and as a fallback, plus a captive-portal DNS server that answers every name with the AP IP. The SSID is derived from `DeviceIdentity` and cannot be configured. Provision mode is one of Always, When WiFi Disconnected, or Never.
- **Install:** `#include <APModule.h>` then `.install<APModule>()` (default constructor, `modules/ap/include/APModule.h:20`). Preset: listed for `.core()` in `lib/ESPRack/include/Builder.h:59-60`, but the preset is an empty stub.
- **Module identity:** id `"ap"`, version `"1.0.0"`, priority 10, no `requires_` (`modules/ap/include/APModule.h:22-26`). Hooks: `onInstall`, `onBegin`, `onLoop` (`:28-34`).
- **Public API:**
  - `APModule::service()` returns `APSettingsService*` (`modules/ap/include/APModule.h:36`).
  - `APSettingsService::getAPNetworkStatus()` (`modules/ap/include/APSettingsService.h:145`).
  - Build-time `FACTORY_AP_*` macros (`modules/ap/include/APSettingsService.h:22-51`).
- **Web/REST/WS:**
  - Typed feature `"ap"` ("Access Point"), feature auth Authenticated. In the menu it nests under `"wifi"` with `nestUnder("wifi")`.
  - Tabs: `status`, and `settings` (postable, admin-only tab) (`modules/ap/src/APSettingsService.cpp:139-162`).
  - Endpoints: `GET /api/v1/ap/form`, `GET`/`POST /api/v1/ap/state`. No WS channel.
- **Persistence:**
  - File `/config/apSettings.json` (`modules/ap/include/APSettingsService.h:54`). ConfigDelegate id `"ap"`, 2048 B buffer (`modules/ap/src/APSettingsService.cpp:112-123`).
  - Keys: `provision_mode, pwd, channel, ssid_hidden, max_clients, local_ip, gateway_ip, subnet_mask` (`modules/ap/include/APSettingsService.h:111-123`). The SSID is not persisted.
- **library.json deps:** `ESPRack` only. Uses the framework `DNSServer`.
- **Cost notes:**
  - The `DNSServer` is heap-allocated only while the AP is running (`modules/ap/src/APSettingsService.cpp:239-244`).
  - `manageAP()` runs every 10 s (`MANAGE_NETWORK_DELAY`, `modules/ap/include/APSettingsService.h:62`). DNS is pumped on every loop.
  - Byte cost not measured.
- **Caveats:**
  - The default AP password is the legacy `"esp-react"` (`modules/ap/include/APSettingsService.h:33`).
  - If the stored password is unusable, `startAP()` silently falls back to the factory password. If that is also unusable, it starts an **open** AP (`modules/ap/src/APSettingsService.cpp:226-237`).
  - It nests under `"wifi"` (`modules/ap/src/APSettingsService.cpp:146`) without declaring `requires_ {"wifi"}`.
  - Feature auth is Authenticated (`modules/ap/src/APSettingsService.cpp:142`). A non-admin user can read the AP password through form or state and can change AP settings (see the auth bugs at the end).

### `auto-update` — AutoUpdateModule
- **Purpose:** Periodically asks an update server over plain HTTP whether new firmware exists. The hard-coded primary server is checked first, then a configurable fallback. On HTTP 200 it flashes the image with `HTTPUpdate` and reboots.
- **Install:** `.install<AutoUpdateModule>()`. Device name and version come from the context (`modules/auto-update/include/AutoUpdateModule.h:15-20`). Preset: listed for `.networking()` in `lib/ESPRack/include/Builder.h:63`, but the preset is an empty stub.
- **Module identity:** id `"autoUpdate"`, version `"1.0.0"`, priority 40, requires `{"wifi"}` (`modules/auto-update/include/AutoUpdateModule.h:11-14`). Hooks: `onInstall`, `onBegin`, `onLoop`.
- **Public API:** None reachable. The module has no `service()` accessor, so `checkForUpdate()` and `onProgress()` (`modules/auto-update/include/AutoUpdateService.h:199-202`) cannot be called by consumers. Build macros: `HARD_UPDATE_SERVER_URL`, `DEFAULT_UPDATE_SERVER_URL` and others (`modules/auto-update/include/AutoUpdateService.h:27-54`).
- **Web/REST/WS:**
  - Typed feature `"autoUpdate"` (Admin): `/api/v1/autoUpdate/form`, `/api/v1/autoUpdate/state`.
  - Adds tab `autoUpdate` (order 80) to the compound `system` feature via `formOf` (`modules/auto-update/src/AutoUpdateService.cpp:104-137`). No WS.
  - Outbound request: `GET <url>?dev=&flv=&ver=&did=[&hw=]` with headers `x-ESP32-STA-MAC` and `x-ESPRack-Device-Id` (`modules/auto-update/src/AutoUpdateService.cpp:13-37, 263-273`).
- **Persistence:** `/config/autoUpdateSettings.json`, id `"autoUpdate"`, 4096 B (`modules/auto-update/src/AutoUpdateService.cpp:58-65`). Keys: `enabled, checkInterval, updateTimeoutSec, serverUrl` (`modules/auto-update/include/AutoUpdateService.h:104-109`).
- **library.json deps:** `ESPRack` only. Uses the framework `HTTPClient` and `HTTPUpdate`.
- **Cost notes:**
  - Creates FreeRTOS task `"ota_update"` per check: 6144 B stack on the heap, priority 1, **pinned to core 1** (`modules/auto-update/src/AutoUpdateService.cpp:200-201`).
  - Plain `WiFiClient`, no TLS (`:244-245, 315-317`).
- **Caveats:**
  - Every device first polls `http://5.58.3.220:5000/update`, a hard-coded public IP over plain HTTP (`modules/auto-update/include/AutoUpdateService.h:27`; `modules/auto-update/src/AutoUpdateService.cpp:342-356`). The module itself checks no image signature.
  - The fallback default is a LAN IP, `http://192.168.10.202:5000/update` (`modules/auto-update/include/AutoUpdateService.h:46`).
  - The task is pinned to core 1 without the `CONFIG_FREERTOS_UNICORE` guard that `modules/telegram/src/TelegramService.cpp:224-231` uses. Behaviour on single-core C3/C6 needs checking.
  - `_state` Strings are written from the OTA task (`setOtaState`/`setLastResult`, `modules/auto-update/src/AutoUpdateService.cpp:221-233`) without the StatefulService lock.
  - On timeout, `vTaskDelete`s the task mid-HTTP and reboots (`:164-179`).
  - The 1–10080 min and 30–600 s ranges are only enforced in the form, not in `updatePersistence` (`modules/auto-update/include/AutoUpdateService.h:111-121`).
  - Dead macros (see the dead-code bug at the end).

### `cert-manager` — CertManagerModule
- **Purpose:** Manages the device PKI:
  - generates an ECDSA-P256 key and a PKCS#10 CSR (CN = `DeviceIdentity::canonical()`);
  - auto-enrolls against `<base>/api/v1/enroll` (bootstrap token or operator approval);
  - loads the certificate and CA into the App TLS context for mTLS;
  - supports `rotate(renewUrl)` and operator-triggered gray-zone recovery through `/api/v1/recover`.
- **Install:** `.install<CertManagerModule>()`. Needs WiFi, and either MothershipModule (URLs come from `IMothershipProfileProvider`) or the `pki_base_url` setting. Preset: none.
- **Module identity:** id `"cert-manager"`, version `"0.1.0"`, priority 12, requires `{"wifi"}` (`modules/cert-manager/include/CertManagerModule.h:19-22`). Hooks:
  - `onInstall`: `app->setCert` and `svc->setApp` (`:34, 41-42`);
  - `onBegin`: binds `app->wireguard()` (`:47-57`);
  - `onLoop`.
- **Public API:**
  - `ICertProvider` (`modules/cert-manager/include/ICertProvider.h:19-64`): `state()`, `hasValidCert()`, `daysUntilExpiry()`, `subjectCN()`, `serialHex()`, `rotate(renewUrl)`. Reached through `app->cert()` (`lib/ESPRack/include/App.h:111-112`).
  - Concrete service: `beginRecovery()` (`modules/cert-manager/include/CertManagerService.h:180`).
  - `CertManagerModule::service()` (`modules/cert-manager/include/CertManagerModule.h:60`).
- **Web/REST/WS:**
  - Typed feature `"certManager"` ("Device PKI", menu "PKI", order 360, Admin): `/api/v1/certManager/form` and `/api/v1/certManager/state`.
  - Tabs: `status` (live), `enrollment`, `settings`, `recovery` (live), `internals` (`modules/cert-manager/src/CertManagerService.cpp:388-463`).
  - Action `cert.beginRecovery` → `POST /api/v1/actions/cert.beginRecovery` (`:371-386`).
  - WS channel `certManager` (`:399`). The `staRead` push carries only status, CN, serial and notAfter (`:314-320`).
- **Persistence:**
  - `/config/cert.json`, ConfigDelegate id `"certManager"`, 8192 B (`modules/cert-manager/include/CertManagerService.h:32`; `modules/cert-manager/src/CertManagerService.cpp:329-349`).
  - Keys: `device_cert_pem, device_key_pem, ca_bundle_pem, serial_hex, subject_cn, not_after_ts, recovery_token, bootstrap_token, pki_base_url` (`:60-83`).
  - The PEMs and both tokens are Secret fields, so they are encrypted at rest.
- **library.json deps:** `ESPRack` only. It also uses framework mbedtls and the core `PersistentTlsClient`, which pulls in `ESP_SSLClient` (`lib/ESPRack/include/PersistentTlsClient.h:52`). That dependency is undeclared.
- **Cost notes:**
  - Three task stacks, each 8192 B and heap-allocated: `certEnroll`, `certRotate`, `certRecover` (`modules/cert-manager/src/CertManagerService.cpp:773-780, 1177-1184, 1340-1347`).
  - `certEnroll` is spawned at boot whenever state ≠ Ready and loops forever waiting for WiFi and a URL (`:819-828`). Without the mothership module or a PKI URL, it stays resident permanently.
  - Each flow uses an on-demand BearSSL client of about 8 KB (comment at `:21-26`), only when `max_alloc ≥ 7168` (`:56`).
  - Per round-trip JSON docs: 2048 B request and 8192 B response (`:990, 1101`). Plus 1 KB PEM buffers on the stack (`:629, 715`).
  - REST buffer AUTO, WS buffer 4096 (`:459-463`).
- **Caveats:**
  - Enroll and recover use `configureInsecure()` (trust on first use) (`:1059, 1517`). The recovery token is sent over TLS that does not verify the server.
  - `parseCertMetadata` feeds UTC certificate times into `mktime` (local time) (`:1663-1670`). The ntp module sets a non-UTC TZ (`modules/ntp/src/NTPSettingsService.cpp:120,126`). If parsing fails, `not_after_ts` = 0 and the state falls to GrayZone (`:878-883, 523, 548-553`).
  - The state machine overwrites its own states (see bugs at the end).
  - Not compiled by the root `library.json` single-library bundle.

### `config-manager-ui` — ConfigManagerUiModule
- **Purpose:** Adds a System → Config Manager tab listing every ConfigManager entry: file, size, save count, snapshot presence, size and age. Provides "Save Backup" (snapshot every config) and "Restore Backup" (restore and reboot).
- **Install:** `.install<ConfigManagerUiModule>()`. Preset: listed for `.full()` (`lib/ESPRack/include/Builder.h:66`), stub.
- **Module identity:** id `"configManagerUi"`, version `"1.0.0"`, priority 85, no deps (`modules/config-manager-ui/include/ConfigManagerUiModule.h:11-13`). Hook: `onInstall` only.
- **Public API:** None.
- **Web/REST/WS:**
  - Page `configManagerUi` with `GET /api/v1/system/configmgr/form` (Admin) (`modules/config-manager-ui/include/ConfigManagerService.h:8`; `modules/config-manager-ui/src/ConfigManagerService.cpp:17-42`).
  - Tab `configmgr` (order 30) on `system` (`:44-53`).
  - Actions `config.snapshot` and `config.restore` (Admin; restore asks for confirmation and reboots) (`:60, 78`).
- **Persistence:** None of its own. It drives `ConfigManager::snapshotAll()` and `restoreAllFromSnapshot()`, which write `<primary>.snapshot` files.
- **library.json deps:** `ESPRack`.
- **Cost notes:** 2560 B response start hint (`:100`). Opens the primary and snapshot file of every entry on each request (`:262-273`). Not measured otherwise.
- **Caveats:**
  - Restore reboots from `r->onDisconnect` with `delay(500); ESP.restart()` inside an async_tcp callback (`:90-93`). It does not call `App::shutdown`.
  - The constructor takes a server and a security manager and ignores both (`:14`).
  - The header documents "snapshot ✓/✗" (`modules/config-manager-ui/include/ConfigManagerService.h:16`), but the rows emit `"yes"`/`"no"` (`modules/config-manager-ui/src/ConfigManagerService.cpp:186`).

### `discovery` — DiscoveryModule
- **Purpose:** Finds sibling ESPRack devices on the LAN.
  - Each device multicasts a text announce to `239.42.7.1:7711` every 8 s. Peers expire after 30 s.
  - Each peer is marked `trusted` (an HMAC of its device id with a shared PSK) and `compatible` (manifest schema `"3"`).
  - The Discovery page shows the result.
- **Install:** `.install<DiscoveryModule>()`. Preset: none.
- **Module identity:** id `"discovery"`, version `"0.1.0"`, priority 24, requires `{"wifi"}` (`modules/discovery/include/DiscoveryModule.h:30-35`). Hooks: `onInstall`, `onBegin`. It has no `onLoop` because it runs its own task.
- **Public API:** `DiscoveryModule::service()` (`:46`). No provider interface.
- **Web/REST/WS:**
  - Page `discovery`, component `"Discovery"`, menu order 1 (`modules/discovery/src/DiscoveryService.cpp:99-107`).
  - `GET /api/v1/discovery/peers` (Authenticated) returns `{self, peers[]}` (`:109-119, 300-333`). No WS.
- **Persistence:** None. The PSK comes from the build-time `FACTORY_DISCOVERY_PSK`, default `"esprack-discovery-dev"` (`:17-24`).
- **library.json deps:** `ESPRack`. Uses the framework `WiFiUdp` and mbedtls md.
- **Cost notes:**
  - Static task `"discovery"` with a 4096 B stack in BSS (`:150-160`).
  - Peer table: 64 × `PeerRow` in BSS. The code comment estimates about 104 B per row and about 6.5 KB total (`:31-38, 56-69`).
  - 320 B announce buffer member (`modules/discovery/include/DiscoveryService.h:76`) and 320 B receive buffer on the task stack (`modules/discovery/src/DiscoveryService.cpp:268`). The receive loop polls every 50 ms.
- **Caveats:**
  - `PeerRow.deviceId` is `char[40]` (`:64`), but the canonical id is the project name plus 22 characters (`lib/ESPRack/include/DeviceIdentity.h:225-226`). Project names longer than 17 characters are truncated. Lookup then compares the truncated stored id against the full incoming id with `strcmp` (`:232, 240`), so every announce from such a peer takes a new slot. The result is duplicate rows until they are pruned or the table fills.
  - Untrusted announces are stored and served too; they are only flagged. When the table is full, new peers are dropped (`:221-252`). Any host on the LAN can fill all 64 slots.
  - The trust tag is static and replayable (`:125-129`). The default PSK is the same for every stock build.
  - Discovery only runs while STA is connected (`:271-275`). The web port is hard-coded to 80 (`modules/discovery/include/DiscoveryService.h:72`).
  - "SWIM-style" here means announce plus timeout only. Probing and gossip are roadmap items (`modules/discovery/include/DiscoveryService.h:9-12, 26-27`).
  - Not in the root `library.json` bundle.

### `factory-reset` — FactoryResetModule
- **Purpose:** Admin action that deletes every file in `/config` and reboots.
- **Install:** `.install<FactoryResetModule>()`. Preset: none.
- **Module identity:** id `"factoryReset"`, version `"1.0.0"`, priority 90, no deps (`modules/factory-reset/include/FactoryResetModule.h:12-14`). Hook: `onInstall` only.
- **Public API:** None reachable (no accessor).
- **Web/REST/WS:** Action `system.factoryReset` → `POST /api/v1/actions/system.factoryReset` (Admin, with confirmation) (`modules/factory-reset/src/FactoryResetService.cpp:14-28`). The System → Resources form links to it (`modules/system-status/src/SystemStatus.cpp:270`).
- **Persistence:** Deletes the contents of `/config` (`modules/factory-reset/include/FactoryResetService.h:17`; `modules/factory-reset/src/FactoryResetService.cpp:38-46`). The `.snapshot` backups there are deleted too.
- **library.json deps:** `ESPRack`. It also includes `RestartService.h` from the restart module (`modules/factory-reset/include/FactoryResetService.h:14`), which is not declared.
- **Cost notes:** Not measured.
- **Caveats:**
  - Deletion is flat, not recursive (the comment at `modules/factory-reset/src/FactoryResetService.cpp:36` admits this). Subdirectories under `/config` survive.
  - It runs inside the request's `onDisconnect` callback (`:31`) and then `restartNow()`. It never calls `App::shutdown`, although `Module.h:29-31` says `onShutdown` runs before a factory reset.

### `features` — FeaturesModule
- **Purpose:** `GET /api/v1/features` returns a flat boolean map of selected installed modules for SPA bootstrap.
- **Install:** `.install<FeaturesModule>()`. Preset: none.
- **Module identity:** id `"features"`, version `"1.0.0"`, priority 5, no deps (`modules/features/include/FeaturesModule.h:18-20`). Hook: `onInstall` only.
- **Public API:** `static FeaturesService::buildFeaturesObject(JsonObject, WebManager*)` (`modules/features/include/FeaturesService.h:33`).
- **Web/REST/WS:**
  - `GET /api/v1/features` is mounted directly on AsyncWebServer with **no auth wrapper** (`modules/features/src/FeaturesService.cpp:7`). It is also registered as a proxy endpoint (`:14-21`).
  - Keys: `project, telegram, mqtt, ntp, ota, upload_firmware, auto_update, system_info, security` (`:35-47`).
- **Persistence:** None.
- **library.json deps:** `ESPRack`.
- **Cost notes:** 512 B start hint (`:54`).
- **Caveats:** The key set is partial. Discovery, cert-manager, mothership, wireguard, wifi, ap, filesystem and the rest are absent, while `manifest.buildFeatures` has the full map.

### `filesystem` — FileSystemModule
- **Purpose:** Multi-volume file manager:
  - volumes: flash LittleFS at `/flash`, plus SD at `/sd` when built with `BOARD_HAS_SD`;
  - list, stat, download, upload (optional `X-File-SHA256` / `X-File-CRC32` check), mkdir, delete, rename;
  - background tar archive and per-file hash jobs; SD format, mount and unmount.
  - `/flash/config` is write-protected.
- **Install:** `.install<FileSystemModule>()`. SD support is compile-time (`BOARD_HAS_SD` and `BOARD_SD_*` macros in `modules/filesystem/include/FileSystemSd.h`). Preset: listed for `.core()`, stub.
- **Module identity:** id `"filesystem"`, version `"1.0.0"`, priority 20, no deps (`modules/filesystem/include/FileSystemModule.h:21-23`). Hooks:
  - `onInstall`;
  - `onBegin` registers the backends, purges debris and adds the read-only prefix (`:29-40`).
- **Public API:**
  - `FileSystemManager` (`modules/filesystem/include/FileSystemManager.h:154-198`) and the `FileSystemBackend` interface (`modules/filesystem/include/FileSystemBackend.h:6-37`).
  - The module exposes neither `manager_` nor `service_` (`modules/filesystem/include/FileSystemModule.h:41-43`).
- **Web/REST/WS:**
  - Page `filesystem`, component `"FileSystem"`, menu order 800, Admin (`modules/filesystem/src/FileSystemService.cpp:100-108`).
  - All endpoints are Admin and live under `/api/v1/fs/` (`modules/filesystem/include/FileSystemService.h:15-35`):
    - GET: `schema`, `volumes`, `list`, `stat`, `download`, `format/status`, `archive/status`, `archive/download`, `hash/status`;
    - POST: `mkdir`, `delete`, `rename`, `format`, `unmount`, `mount`, `archive/prepare`, `archive/cancel`, `hash/start`, `hash/cancel`.
  - `POST upload` (multipart) is bound directly in the constructor (`modules/filesystem/src/FileSystemService.cpp:76-78`) and checks `IS_ADMIN` itself (`:430-435`).
- **Persistence:** None.
- **library.json deps:** `ESPRack`. Uses the framework LittleFS, SD/SD_MMC and mbedtls sha256.
- **Cost notes:** All tasks are created on demand and pinned to core 0:
  - `fs_format`: 4096 B (`modules/filesystem/src/FileSystemService.cpp:693-707`);
  - `fs_archive_walk`: 6144 B (`:936-937`);
  - `fs_hash`: 6144 B, plus a static 4096 B BSS buffer (`:1071, 1131-1132`);
  - `sd_mount_w`: 8192 B (`modules/filesystem/include/FileSystemSd.h:80-81`).
  - The archive entry list is kept in RAM (`ArchiveJob`, `modules/filesystem/include/FileSystemService.h:49-61`).
- **Caveats:**
  - The boot purge deletes **every zero-byte file** anywhere on flash, not only upload debris (`modules/filesystem/src/FileSystemManager.cpp:199`; called from `modules/filesystem/include/FileSystemModule.h:32`).
  - `/flash/config` is protected against writes only. `openRead` and `list` do not check the read-only prefix (`modules/filesystem/src/FileSystemManager.cpp:232-239`).
  - `sanitize()` rejects `/..` but not `.` segments (`:10-20`), so `/flash/./config/...` bypasses the prefix match. Whether LittleFS then normalises the path was not verified.
  - `adoptMounted()` marks flash as mounted unconditionally, even if `App` failed to mount it (`modules/filesystem/include/FileSystemFlash.h:35`). `FileSystemFlash::mount()` formats on failure (`:17`), against App's no-format-first policy (`lib/ESPRack/src/App.cpp:210-230`).
  - Upload state is one set of member fields (`modules/filesystem/include/FileSystemService.h:83-86`), so concurrent uploads collide.
  - SD "format" is a recursive delete. Formatting flash is refused (`modules/filesystem/src/FileSystemService.cpp:683-687`).

### `mothership` — MothershipModule
- **Purpose:** Fleet-management client.
  - Periodically POSTs over HTTPS (BearSSL, mTLS using the App TLS context) to `<active profile>/api/v1/checkin`. The body carries identity, heap figures, WireGuard status and pending action results.
  - Executes actions queued by the server: `renewCert`, `openTunnel`/`closeTunnel`, `setCadence`, `rest.proxy`, `config.dump`/`config.restore`, `reboot`, `log`. `update` and `setConfig` are stubs.
  - Holds two named endpoint profiles that also feed the cert-manager URLs.
- **Install:** `.install<MothershipModule>()` together with CertManagerModule and WiFiModule. WireGuardModule is optional. Preset: none.
- **Module identity:** id `"mothership"`, version `"0.1.0"`, priority 35, requires `{"wifi","cert-manager"}` (`modules/mothership/include/MothershipModule.h:25-28`). Hooks:
  - `onInstall`: `setMothership` and `setMothershipProfile` (`:42, 48`);
  - `onBegin`;
  - `onLoop` (empty body, `modules/mothership/src/MothershipService.cpp:365-369`).
- **Public API:**
  - `IMothershipProvider` (`modules/mothership/include/IMothershipProvider.h:21-41`) through `app->mothership()`.
  - `IMothershipProfileProvider` (`lib/ESPRack/include/IMothershipProfileProvider.h:26-48`) through `app->mothershipProfile()`.
  - `static MothershipService::lastSentHeap()` (`modules/mothership/include/MothershipService.h:199`), used by system-status.
  - `service()` (`modules/mothership/include/MothershipModule.h:56`).
- **Web/REST/WS:**
  - Typed feature `"mothership"` (menu order 370, Admin), tabs `status` (not live) and `settings` (`modules/mothership/src/MothershipService.cpp:252-307`): `/api/v1/mothership/form`, `/api/v1/mothership/state`.
  - No WS channel.
  - `rest.proxy` lets the server call any registered entry's in-process handler. By design this **bypasses JWT auth**; the trust boundary is the mTLS check-in (`:1082-1086, 1149-1150`).
- **Persistence:**
  - `/config/mothership.json`, id `"mothership"`, 4096 B (`modules/mothership/include/MothershipService.h:32`; `modules/mothership/src/MothershipService.cpp:216-230`).
  - Keys: `enabled, interval_s, active_idx, profile_0_name, profile_0_url, profile_1_name, profile_1_url` (`:39-47`). The legacy `interval_min` is migrated (`:58-73`).
  - Defaults: "Home" = `https://mothership.local:8443`, "Work" empty, interval 60 s (`modules/mothership/include/MothershipService.h:41, 49, 93-94`).
- **library.json deps:** `ESPRack` only. Needs `ESP_SSLClient` through the core `PersistentTlsClient` (undeclared).
- **Cost notes:**
  - Static task `"mothership"`, 8192 B stack in BSS (`modules/mothership/src/MothershipService.cpp:344-356`).
  - Members `StaticJsonDocument<3072>` and `<4096>`, a `PersistentTlsClient`, and 2 × 512 B reserved Strings (`modules/mothership/include/MothershipService.h:215, 329, 336`; `modules/mothership/src/MothershipService.cpp:327-328`). These are heap-resident because the service is created with `new` (`modules/mothership/include/MothershipModule.h:37`).
  - Result ring: at most 16 × 1 KB (`modules/mothership/include/MothershipService.h:347-351`).
  - Transient allocations: 8 KB manifest-CRC doc once (`modules/mothership/src/MothershipService.cpp:636`); 24 KB plus 4 KB docs per `rest.proxy` (`:1118-1143`); 32 KB doc per `config.dump` (`:1306-1315`). The last two are gated on `max_alloc`.
- **Caveats:**
  - `update` is a stub that returns "deferred to phase 2.5" (`:944-958`). `setConfig` only acknowledges (`:1026-1036`).
  - `reboot` and `config.restore` call `esp_restart()` directly (`:1038-1058, 1343-1361`), with no `App::shutdown`.
  - The default `.local` base URL conflicts with core comments (see the mDNS bullet at the end).
  - The ring and chunking bugs are listed at the end.
  - Not in the root `library.json` bundle.

### `mqtt` — MqttModule
- **Purpose:** MQTT broker connection through AsyncMqttClient over plain TCP.
  - Other services get subscription handles: topic prefix, QoS/retain defaults, a per-minute publish cap, and wildcard inbound dispatch.
  - The legacy `IMqttDispatcher` callbacks still work.
  - The UI has an operator "sniffer" and persistent "attached" topics.
- **Install:** `.install<MqttModule>()`. Preset: listed for `.full()`, stub.
- **Module identity:** id `"mqtt"`, version `"2.0.0"`, priority 40, requires `{"wifi"}` (`modules/mqtt/include/MqttModule.h:12-15`). Hooks: `onInstall` (`app->setMqtt`, `:22`), `onBegin`, `onLoop`.
- **Public API:**
  - `IMqttProvider` (`modules/mqtt/include/IMqttProvider.h:14-52`) through `app->mqtt()`:
    - `subscribe(name, MqttSubscriptionConfig)` returns an `MqttSubscription` handle with `publish`, `publishTo`, `onMessage`, `active`, `stats`, `setEnabled`, `unsubscribe` (`modules/mqtt/include/MqttSubscription.h:13-49`);
    - `attachedTopics()`.
  - Config struct `modules/mqtt/include/MqttTypes.h:20-53`.
  - Legacy `IMqttDispatcher` (`lib/ESPRack/include/IMqttDispatcher.h:11-17`).
  - `service()` (`modules/mqtt/include/MqttModule.h:30`).
- **Web/REST/WS:**
  - Typed feature `"mqtt"` (menu order 350, **Authenticated**), tabs `status` (live), `subs`, `settings` (admin tab) (`modules/mqtt/src/MqttSettingsService.cpp:200-236`): `/api/v1/mqtt/form`, `/api/v1/mqtt/state`.
  - Actions `mqtt.attachTopic` and `mqtt.detachTopic` (Admin; topic passed as `?topic=`) (`:149-198`).
  - WS channel `mqtt` (`:210`). `staRead` streams seen-topic deltas (`modules/mqtt/include/MqttSettingsService.h:177-224`).
- **Persistence:**
  - `/config/mqttSettings.json`, id `"mqtt"`, 4096 B (`modules/mqtt/include/MqttSettingsService.h:56`; `modules/mqtt/src/MqttSettingsService.cpp:72-83`).
  - Keys: `enabled, host, port, username, pwd, keep_alive, clean_session, max_topic_length, attached_topics[], sniffer_pattern` (`modules/mqtt/include/MqttSettingsService.h:159-174`).
  - Defaults: disabled, `test.mosquitto.org:1883` (`:24-31`). The client id is always `DeviceIdentity::canonical()` (`modules/mqtt/src/MqttSettingsService.cpp:334`).
- **library.json deps:** `ESPRack` only. Needs `AsyncMqttClient` (`modules/mqtt/include/MqttSettingsService.h:15`), which is declared only in the root `library.json:49-52`.
- **Cost notes:**
  - AsyncMqttClient member. REST buffer AUTO, WS buffer 4096 (`modules/mqtt/src/MqttSettingsService.cpp:318-322`).
  - Seen-topic ring: at most 50 entries, payload truncated to 48 characters. Pending list at most 32. At most 16 attached topics (`modules/mqtt/include/MqttSettingsService.h:62-64, 131`).
  - No task of its own; it runs in async_tcp callbacks and `loop()`. Not measured.
- **Caveats:**
  - No TLS: `configureMqtt` never calls `setSecure` (`modules/mqtt/src/MqttSettingsService.cpp:716-737`).
  - `staRead` echoes the broker password (`modules/mqtt/include/MqttSettingsService.h:198`). It also clears `pendingSeenTopics` (`:222`). Because `GET /state` uses the WS reader (`lib/ESPRack/include/WebFeatureDelegate.h:98-99`), a REST GET "steals" rows from WS subscribers.
  - The inbound callback mutates `_state` on async_tcp without the StatefulService lock (`modules/mqtt/src/MqttSettingsService.cpp:438-483`).
  - `_discoveredTopics` evicts the alphabetically first topic, not the oldest (`:973-977`).
  - Only single-frame inbound messages reach subscription handlers (`:473`).

### `ntp` — NTPModule
- **Purpose:** Runs the SNTP client once STA has an IP and shows a live status tab with local time, UTC time and uptime. Since SES-LIB-002 (version 1.1.0): SNTP results go through core `TimeSync`, which decides whether they may step the clock; the timezone is handed to `TimeSync::setTimezone()`; the SNTP interval is 1 h; the status tab adds a clock line ("Valid, ntp, ±1 s"), a sync line ("last step … ms, N rejected") and an admin "Set time" control (`system.setTime`).
- **Install:** `.install<NTPModule>()`. Preset: listed for `.networking()`, stub.
- **Module identity:** id `"ntp"`, version `"1.0.0"`, priority 30, requires `{"wifi"}` (`modules/ntp/include/NTPModule.h:11-14`). Hooks: `onInstall`, `onBegin`, `onLoop`.
- **Public API:** None (no accessor).
- **Web/REST/WS:**
  - Typed feature `"ntp"` (menu order 200, **Authenticated**), tabs `status` (live) and `settings` (admin tab) (`modules/ntp/src/NTPSettingsService.cpp:35-77`): `/api/v1/ntp/form`, `/api/v1/ntp/state`.
  - WS channel `ntp`, pushed at 1 Hz only while someone is subscribed (`:85-96`).
- **Persistence:** `/config/ntpSettings.json`, id `"ntp"`, 2048 B (`modules/ntp/include/NTPSettingsService.h:56`; `modules/ntp/src/NTPSettingsService.cpp:4-13`). Keys: `enabled, server, tz_label, tz_format`. Defaults: `time.google.com`, Europe/London (`modules/ntp/include/NTPSettingsService.h:43-53`).
- **library.json deps:** `ESPRack`.
- **Cost notes:** WS buffer 2048, REST buffer AUTO (`modules/ntp/src/NTPSettingsService.cpp:72-76`). Not measured otherwise.
- **Caveats:**
  - The WS updater is the full `update()` (`modules/ntp/include/NTPSettingsService.h:116-118`), so any authenticated WS client can change the server and TZ.
  - `configureNTP` runs from the WiFi event task (`modules/ntp/src/NTPSettingsService.cpp:99-106`).
  - The process TZ (now owned by `TimeSync`) affects cert-manager's `mktime` (see cert-manager).

### `ota` — OTAModule
- **Purpose:** ArduinoOTA listener for espota / PlatformIO network uploads, with configurable port and password. ArduinoOTA's own mDNS responder is disabled.
- **Install:** `.install<OTAModule>()`. Preset: listed for `.networking()`, stub.
- **Module identity:** id `"ota"`, version `"1.0.0"`, priority 40, requires `{"wifi"}` (`modules/ota/include/OTAModule.h:11-14`). Hooks: `onInstall`, `onBegin`, `onLoop`.
- **Public API:** None.
- **Web/REST/WS:** Typed feature `"ota"` (Admin): `/api/v1/ota/form`, `/api/v1/ota/state`. Tab `ota` (order 60) on `system` (`modules/ota/src/OTASettingsService.cpp:39-70`).
- **Persistence:** `/config/otaSettings.json`, id `"ota"`, 1024 B (`modules/ota/include/OTASettingsService.h:30`; `modules/ota/src/OTASettingsService.cpp:15-24`). Keys: `enabled, port, pwd`. Defaults: enabled, port 8266, password `"esp-react"` (`modules/ota/include/OTASettingsService.h:19-27`).
- **library.json deps:** `ESPRack`. Uses framework ArduinoOTA.
- **Cost notes:** `ArduinoOTAClass` is heap-allocated once STA has an IP (`modules/ota/src/OTASettingsService.cpp:99-104`). Not measured.
- **Caveats:**
  - OTA is enabled by default with the well-known password `"esp-react"`.
  - `update()` fills in factory defaults for any missing key (`modules/ota/include/OTASettingsService.h:57-59`). A partial POST, such as only `{"enabled":false}` or a form that leaves out the unchanged secret `pwd`, resets the port and **password** to the defaults.
  - Race: `configureArduinoOTA()` deletes and recreates `_arduinoOTA` from the GOT_IP event task (`modules/ota/src/OTASettingsService.cpp:26, 84-90, 139-141`) while `loop()` calls `_arduinoOTA->handle()` on loopTask (`:77-80`). There is no lock.
  - OTA is not available while the device is in AP-only provisioning (`:99-101`).
  - The progress log divides by `total/100` (`:119`), which divides by zero when total < 100.

### `presence` — PresenceModule
- **Purpose:** In-RAM registry of active SPA browser tabs, keyed by the `X-Client-Key` header.
  - It is fed by every request that goes through the SecurityManager wrapper, and by a dedicated `/ws/presence` socket that receives `hello` and `page` frames.
  - System → Clients (ws-diag) displays it.
- **Install:** `.install<PresenceModule>()`. Required by SecurityModule and WsDiagModule. Preset: none, and the documented `.core()` set does not include it.
- **Module identity:** id `"presence"`, version `"1.0.0"`, priority 7, no deps (`modules/presence/include/PresenceModule.h:16-18`). Hooks:
  - `onInstall`: `app->setPresence` and `mountWs` (`:19-23`);
  - `onLoop`: `purgeStale` every 30 s (`:24-32`).
- **Public API:** `PresenceService`, reached through `ctx.presence` or `app->presence()` (`modules/presence/include/PresenceService.h:75-97`): `touchFromRequest`, `touch`, `bindWsTransport`, `unbindWsTransport`, `purgeStale`, `fillStats`, `clientCount`, `setLogSink`, `setLogEnabled`.
- **Web/REST/WS:** Its own `AsyncWebSocket` at `/ws/presence` (`modules/presence/include/PresenceService.h:14`; `modules/presence/src/PresenceService.cpp:16-94`). The handshake requires `IS_AUTHENTICATED` (`:84-91`). No REST or manifest entries of its own.
- **Persistence:** None. An optional log sink `/logs/presence.log` exists but is never enabled.
- **library.json deps:** `ESPRack`.
- **Cost notes:**
  - One extra WS handler, outside the `/ws/app` multiplexing.
  - `std::map` of clients, each with up to 10 history entries; idle entries evicted after 10 min (`modules/presence/include/PresenceService.h:48-51`).
  - `StaticJsonDocument<512>` on the async_tcp stack for each WS frame (`modules/presence/src/PresenceService.cpp:40`). Not measured.
- **Caveats:**
  - The handshake predicate captures the **NullSecurityManager** (see the install-order bug at the end), so `/ws/presence` is effectively unauthenticated.
  - `upsertLocked` takes no lock (`:134-153`). `_clients` is written from async_tcp and erased from loopTask.
  - Entries are keyed by an arbitrary client-supplied header (`:183-200`).

### `restart` — RestartModule
- **Purpose:** `system.restart` admin action, plus the static `RestartService::restartNow()` helper used by factory-reset and upload-firmware.
- **Install:** `.install<RestartModule>()`. Preset: none.
- **Module identity:** id `"restart"`, version `"1.0.0"`, priority 90, no deps (`modules/restart/include/RestartModule.h:11-13`). Hook: `onInstall` only.
- **Public API:** `static RestartService::restartNow()` (`modules/restart/include/RestartService.h:27-32`).
- **Web/REST/WS:** `POST /api/v1/actions/system.restart` (Admin, with confirmation) (`modules/restart/src/RestartService.cpp:10-24`). Linked from System → Resources (`modules/system-status/src/SystemStatus.cpp:269`).
- **Persistence:** None.
- **library.json deps:** `ESPRack`.
- **Cost notes:** Negligible. Not measured.
- **Caveats:** `restartNow` runs in the request's `onDisconnect` callback (async_tcp) with `WiFi.disconnect(true)` and `vTaskDelay(500)` (`modules/restart/include/RestartService.h:27-31`; `modules/restart/src/RestartService.cpp:27`). It never calls `App::shutdown`.

### `security` — SecurityModule
- **Purpose:** The real `SecurityManager`:
  - users with PBKDF2-SHA256 password hashes and JWT sign-in;
  - role predicates;
  - machine API tokens (reader/operator/admin) with revocation.
  - It replaces App's `NullSecurityManager` and installs the upgrade auth filter on `/ws/app`.
- **Install:** `.install<SecurityModule>()`, which needs PresenceModule. Preset: listed for `.core()`, stub.
- **Module identity:** id `"security"`, version `"1.0.0"`, priority 8, requires `{"presence"}` (`modules/security/include/SecurityModule.h:33-38`). Hooks:
  - `onInstall`: sets the SecurityManager on App and WebManager, wires presence, sets the WS auth filter, registers the manifest and `AuthenticationService` (`:40-69`);
  - `onBegin`.
- **Public API:**
  - `SecuritySettingsService`, which implements `SecurityManager`: `authenticate`, `authenticateRequest`, `generateJWT`, `filterRequest`, `wrapRequest`, `wrapCallback`, `mintMachineToken`, `revokeMachineToken` (`modules/security/include/SecuritySettingsService.h:126-146`).
  - `SecurityModule::service()` (`modules/security/include/SecurityModule.h:76`).
- **Web/REST/WS:**
  - `POST /api/v1/auth/token` (body `{username,pwd}`, at most 256 B) and `GET /api/v1/auth/verify`. Both are mounted directly and also as proxy endpoints (`modules/security/include/AuthenticationService.h:11-14`; `modules/security/src/AuthenticationService.cpp:13-44`).
  - Typed features, all Admin:
    - `security`: `/api/v1/security/form|state` (`modules/security/src/SecuritySettingsService.cpp:185-203`);
    - `securityJwt`: system tab `jwt`, order 50 (`:211-237`);
    - `apiTokens`: system tab `tokens`, order 55 (`:295-314`).
  - Actions `tokens.create` and `tokens.revoke` (Admin) (`:243-290`).
- **Persistence:**
  - `/config/securitySettings.json`, id `"security"`, 2048 B (`modules/security/include/SecuritySettingsService.h:37`; `modules/security/src/SecuritySettingsService.cpp:154-165`).
  - Contents: `jwt_secret`, `users[{username,pwd(hash),salt,admin}]`, `api_tokens[{tid,name,role,revoked}]`, with at most 8 tokens (`modules/security/include/SecuritySettingsService.h:63`).
  - Factory users `admin`/`admin` and `guest`/`guest` (`:22-34`; `modules/security/src/SecuritySettingsService.cpp:118-130`). JWT secret seeded from `"#{random}-#{random}"` (`modules/security/include/SecuritySettingsService.h:18`).
- **library.json deps:** `ESPRack`.
- **Cost notes:** PBKDF2 with 2000 iterations on every login (`lib/ESPRack/include/SecurityHash.h:16`). Not measured.
- **Caveats:**
  - Human JWTs get an `exp` only when the clock is sane. Tokens minted before NTP sync never expire (`modules/security/src/SecuritySettingsService.cpp:505-512`).
  - Machine-token `tid` is 32 random bits (`:451-454`).
  - `tokens.create` builds its JSON by string concatenation with the operator-supplied name unescaped (`:265-266`).
  - The proxy `GET /api/v1/auth/verify` always returns 200 (`modules/security/src/AuthenticationService.cpp:38-43`).
  - The default `guest` account and the `IS_OPERATOR` rule are listed as bugs at the end.

### `system-status` — SystemStatusModule
- **Purpose:**
  - System → Identity tab: device id, project, versions, chip, MAC, UIDs.
  - System → Resources tab: three heap views, PSRAM, sketch, flash, filesystem, a FreeRTOS task stack high-water table, and restart / factory-reset buttons.
  - A flat machine-health JSON endpoint.
- **Install:** `.install<SystemStatusModule>()`. Preset: listed for `.core()`, stub.
- **Module identity:** id `"systemStatus"`, version `"1.0.0"`, priority 80, no deps (`modules/system-status/include/SystemStatusModule.h:11-13`). Hook: `onInstall` only.
- **Public API:** None reachable.
- **Web/REST/WS:**
  - Page `systemStatus` (Authenticated) with `GET /api/v1/system/health`, `/api/v1/system/status/form` and `/api/v1/system/identity/form` (`modules/system-status/include/SystemStatus.h:20-22`; `modules/system-status/src/SystemStatus.cpp:41-94`).
  - Tabs `identity` (order 10) and `status`, titled "Resources" (order 12), on `system` (`:99-119`).
- **Persistence:** None.
- **library.json deps:** `ESPRack`. There is also an undeclared compile-time coupling to `MothershipService.h` through `__has_include` (`:18-23, 190-205`).
- **Cost notes:** Allocates a `TaskStatus_t[]` per request (`:242-261`). 1792 B start hint for `/health` (`:332`). The proxy handler uses a 4096 B scratch doc (`:59`). Not measured.
- **Caveats:**
  - The proxy `internalHandler` for `/api/v1/system/health` returns the form's `status` section, not the flat JSON the HTTP handler sends (`:52-65` vs `:273-332`).
  - `hw_suffix` is computed locally without the octal-PSRAM `v` suffix (`:311-315`). AutoUpdate's `flv` uses `DeviceIdentity::hwSuffix()`, which appends `v` (`lib/ESPRack/include/DeviceIdentity.h:312-320`; `modules/auto-update/src/AutoUpdateService.cpp:47-53`).
  - The Resources form links `system.restart` and `system.factoryReset` (`:269-270`) without depending on those modules.

### `telegram` — TelegramModule
- **Purpose:** Sends messages through the Telegram Bot API.
  - Messages are queued and sent by a FreeRTOS worker.
  - Other services get subscription handles: default chat/topic, tag prefix, per-minute cap.
  - The UI has a manual composer and a rolling send log.
- **Install:** `.install<TelegramModule>()`. Preset: listed for `.full()`, stub.
- **Module identity:** id `"telegram"`, version `"2.0.0"`, priority 40, requires `{"wifi"}` (`modules/telegram/include/TelegramModule.h:12-14`). Hooks: `onInstall` (`app->setTelegram`, `:23`) and `onBegin`.
- **Public API:**
  - `ITelegramProvider` (`modules/telegram/include/ITelegramProvider.h:14-44`) through `app->telegram()`:
    - `subscribe(name, TelegramSubscriptionConfig)` returns a `TelegramSubscription` with `send`, `sendTo`, `stats`, `enabled`/`setEnabled`, `active`, `unsubscribe` (`modules/telegram/include/TelegramSubscription.h:26-70`).
  - Legacy `enqueueDefault` and `enqueueMessage` (`modules/telegram/include/TelegramService.h:69-74`).
  - `service()` (`modules/telegram/include/TelegramModule.h:30`).
- **Web/REST/WS:**
  - Typed feature `"telegram"` (menu order 400, Admin), tabs `status` (live), `subs`, `settings` (`modules/telegram/src/TelegramService.cpp:67-103`): `/api/v1/telegram/form`, `/api/v1/telegram/state`.
  - Action `telegram.sendManual` (`:54-65`).
  - WS channel `telegram` (`:77`). WS writes are limited to `manualText` (`modules/telegram/src/TelegramSettings.cpp:173, 206`).
- **Persistence:** `/config/telegram.json`, id `"telegram"`, 4096 B (`modules/telegram/include/TelegramService.h:26`; `modules/telegram/src/TelegramService.cpp:9-20`). Keys: `token, chat, topic, ena, interval_ms, parse_mode, silent_default` (`modules/telegram/src/TelegramSettings.cpp:10-19`). The token is a Secret field (`:135`).
- **library.json deps:** `ESPRack` only. Needs `UniversalTelegramBot` (`modules/telegram/include/TelegramService.h:15`), which is declared only in the root `library.json:53-56` as a fork URL.
- **Cost notes:**
  - Static task `"TelTask"`, 6144 B stack in BSS (`modules/telegram/src/TelegramService.cpp:220-231`).
  - Queue of 20 pointers, each message heap-allocated (`modules/telegram/include/TelegramService.h:27`; `modules/telegram/src/TelegramService.cpp:33`).
  - Members: `WiFiClientSecure` (mbedtls) and `UniversalTelegramBot` (`modules/telegram/include/TelegramService.h:111-112`).
  - REST and WS buffers fixed at 8192 / 8192 (`modules/telegram/src/TelegramService.cpp:166-170`).
  - `StaticJsonDocument<1024>` on the task stack per send (`:514`). Log of at most 30 entries (`modules/telegram/include/TelegramSettings.h:75`).
- **Caveats:**
  - TLS uses `setInsecure()` (`modules/telegram/src/TelegramService.cpp:175`), so the bot token goes out without server verification.
  - The worker task starts only if Telegram is enabled at boot (`:220-235`). Enabling it at runtime queues messages that are never sent until a reboot.
  - Text longer than about 1 KB overflows the 1024 B body document (`:514-516`), and the `text` field is silently dropped.
  - The worker mutates `_state` and calls `callUpdateHandlers` from its own task (`:670-672`).

### `ui-dyn` — UiDynModule
- **Purpose:** Legacy "controls" registry served at `GET /api/v1/ui/dynamic` as `{schemaVersion:3, controls[]}`.
- **Install:** `.install<UiDynModule>()`. Preset: none.
- **Module identity:** id `"uiDyn"`, version `"1.0.0"`, priority 6, no deps (`modules/ui-dyn/include/UiDynModule.h:14-16`). Hooks: `onInstall` (create the service and register the proxy), `onBegin` (mount the GET).
- **Public API:** `UiDynService::registerControl` and related methods (`modules/ui-dyn/include/UiDynService.h:20-127`). The module has no accessor, and nothing in `lib/` or `modules/` calls `registerControl`. The endpoint therefore always returns an empty `controls` array.
- **Web/REST/WS:** `GET /api/v1/ui/dynamic`, mounted directly (`modules/ui-dyn/src/UiDynService.cpp:31-52`) and as a proxy (`:54-68`).
- **Persistence:** None.
- **library.json deps:** `ESPRack`.
- **Cost notes:** Fixed array of 16 `ControlInfo` (7 Strings each) inside the service (`modules/ui-dyn/include/UiDynService.h:23, 124`). 4096 B start hint (`:16-18`).
- **Caveats:**
  - It installs at priority 6, before security, and captures the NullSecurityManager (`modules/ui-dyn/include/UiDynModule.h:18`). The "JWT protected" route is therefore public.
  - Its comment calls it "the React app's source of truth for menu / routes / tabs" (`:5-6`), but the real manifest is `/api/v1/ui/manifest`.
  - Ukrainian-language comments at `modules/ui-dyn/include/UiDynService.h:37, 84`.
  - Looks like dead code.

### `upload-firmware` — UploadFirmwareModule
- **Purpose:** Browser upload of a `.bin` firmware image (multipart), written with Arduino `Update`, followed by a reboot.
- **Install:** `.install<UploadFirmwareModule>()`. Preset: listed for `.networking()`, stub.
- **Module identity:** id `"uploadFirmware"`, version `"1.0.0"`, priority 40, no deps (`modules/upload-firmware/include/UploadFirmwareModule.h:11-13`). Hook: `onInstall` only.
- **Public API:** `static UploadFirmwareService::buildForm(JsonObject&)` (`modules/upload-firmware/include/UploadFirmwareService.h:33`).
- **Web/REST/WS:**
  - `POST /api/v1/system/upload/firmware` (multipart), bound directly (`modules/upload-firmware/src/UploadFirmwareService.cpp:13-23`), with the admin check inside the chunk handler (`:101-115`).
  - `GET /api/v1/system/upload/form` (Admin) (`:42-67`).
  - Tab `upload` (order 70) on `system` (`:69-76`).
- **Persistence:** None. It writes the OTA partition.
- **library.json deps:** `ESPRack`. Includes `RestartService.h` from the restart module (`modules/upload-firmware/include/UploadFirmwareService.h:17`), undeclared.
- **Cost notes:** Not measured.
- **Caveats:**
  - No image signature or hash check beyond what `Update` itself does (`:120-131`).
  - `Update.begin(request->contentLength())` passes the length of the whole multipart body (`:104`).
  - The error marker is `new int` stored in `_tempObject` (`:148`). The AsyncWebServer library is believed to release `_tempObject` with `free()`; not verified in this repo.
  - Reboots via `onDisconnect(restartNow)` (`:136`), with no `App::shutdown`.

### `web-endpoints` — WebEndpointsModule
- **Purpose:** System → Endpoints tab: registry summary by kind, WS aggregate health (clients, RTT, keepalive, backpressure) and a per-entry table. The table is streamed as a chunked response.
- **Install:** `.install<WebEndpointsModule>()`. Preset: listed for `.full()`, stub.
- **Module identity:** id `"webEndpoints"`, version `"1.0.0"`, priority 85, no deps (`modules/web-endpoints/include/WebEndpointsModule.h:11-13`). Hook: `onInstall` only.
- **Public API:** None.
- **Web/REST/WS:** `GET /api/v1/system/endpoints/form` (Admin) (`modules/web-endpoints/include/WebEndpointsService.h:7`; `modules/web-endpoints/src/WebEndpointsService.cpp:20-45`). Tab `endpoints` (order 40) on `system` (`:47-55`).
- **Persistence:** None.
- **library.json deps:** `ESPRack`.
- **Cost notes:** Each request uses a chunker with 2048 B and 512 B scratch docs (`:270-271`) and a 2048 B WS-stats doc (`:148`). Output is capped at 64 KB (`:269`).
- **Caveats:** None specific to this module; its stale header comment is listed at the end.

### `wifi` — WiFiModule
- **Purpose:** STA connection management:
  - primary and secondary credential slots, with failover after 2 consecutive disconnects;
  - optional static IP and hostname;
  - async network scan;
  - Status, Scan and Settings tabs.
- **Install:** `.install<WiFiModule>()`. Preset: listed for `.core()`, stub.
- **Module identity:** id `"wifi"`, version `"1.0.0"`, priority 10, no deps (`modules/wifi/include/WiFiModule.h:25-29`). Hooks: `onInstall`, `onBegin`, `onLoop`.
- **Public API:** `WiFiModule::service()` (`:44`). `WiFiSettingsService::activeAp()`, `setScanInProgress()`, `isScanInProgress()` (`modules/wifi/include/WiFiSettingsService.h:118-130`).
- **Web/REST/WS:**
  - Typed feature `"wifi"` (menu order 140, **Authenticated**), tabs `status`, `scan` (admin tab), `settings` (admin tab) (`modules/wifi/src/WiFiSettingsService.cpp:402-456`): `/api/v1/wifi/form`, `/api/v1/wifi/state`.
  - Action `wifi.scan` (Admin, returns 202) (`:389-400, 686-702`). No WS.
- **Persistence:**
  - `/config/wifiSettings.json`, id `"wifi"`, 2048 B (`modules/wifi/include/WiFiSettingsService.h:33`; `modules/wifi/src/WiFiSettingsService.cpp:320-331`).
  - Keys: `ssid, pwd, ssid_2, pwd_2, hostname, static_ip_config, local_ip, gateway_ip, subnet_mask, dns_ip_1, dns_ip_2` (`modules/wifi/include/WiFiSettingsService.h:81-93`).
  - Hostname default `"#{platform}-#{unique_id}"` (`:28`).
- **library.json deps:** `ESPRack`.
- **Cost notes:**
  - REST buffer fixed at 6144 B (`modules/wifi/src/WiFiSettingsService.cpp:445-456`).
  - Reconnect cycle every 30 s (`modules/wifi/include/WiFiSettingsService.h:41`).
  - Modem sleep disabled; the code comment estimates about 20 mA extra (`modules/wifi/src/WiFiSettingsService.cpp:347-354`).
- **Caveats:**
  - The constructor, which runs during `onInstall`, already reconfigures the radio: `WiFi.mode(OFF/NULL)`, `persistent(false)`, `setAutoReconnect(false)`, `setSleep(false)` (`:334-354`). This contradicts `Module.h:16-18` ("WiFi/peripherals are NOT YET UP").
  - Feature auth is Authenticated, so STA passwords are exposed to non-admin users (see the auth bugs at the end).

### `wireguard` — WireGuardModule
- **Purpose:** On-demand WireGuard tunnel to the mothership.
  - Generates a Curve25519 keypair and persists it.
  - Brings the tunnel up or down when MothershipService dispatches `openTunnel` / `closeTunnel`.
  - Its public key and triplet are reported in check-ins and enrollment.
- **Install:** `.install<WireGuardModule>()`. The class is `WireGuardModule`, declared in `WireguardModule.h`. It needs the external `<WireGuard-ESP32.h>` header. Without it the module compiles as a stub whose `up()` returns false (`modules/wireguard/src/WireguardService.cpp:24-29, 463-467`). Preset: none.
- **Module identity:** id `"wireguard"`, version `"0.2.0"` (`WIREGUARD_MODULE_VERSION`), priority 14, requires `{"wifi","cert-manager"}` (`modules/wireguard/include/WireguardModule.h:28-37`). Hooks: `onInstall` (`app->setWireguard`, `:43`), `onBegin`, `onLoop`.
- **Public API:**
  - `IWireguardProvider` (`modules/wireguard/include/IWireguardProvider.h:21-91`): `publicKey`, `isUp`, `hasTriplet`, `assignedIp`, `lastHandshakeAgeSec`, `txBytes`, `rxBytes`, `up(serverPub, endpoint, ip)`, `down()`. Reached through `app->wireguard()` (`lib/ESPRack/include/App.h:133-134`).
  - `service()` (`modules/wireguard/include/WireguardModule.h:50`).
- **Web/REST/WS:**
  - Typed feature `"wireguard"` (menu "Tunnel", order 380, Admin), tabs `status` (`live=true`) and `internals` (`modules/wireguard/src/WireguardService.cpp:198-238`): `/api/v1/wireguard/form`, `/api/v1/wireguard/state`.
  - **No WS channel**, because `wsPath` is never set, even though `staRead`/`staUpd` and a 2048 B WS buffer are registered (`:233-237`).
- **Persistence:** `/config/wireguard.json`, id `"wireguard"`, 2048 B (`modules/wireguard/include/WireguardService.h:46`; `modules/wireguard/src/WireguardService.cpp:180-191`). Keys: `priv_key` (Secret, encrypted), `pub_key`, `server_pub_key`, `endpoint`, `assigned_ip` (`:43-56`).
- **library.json deps:** `ESPRack` and `CertManagerModule` (`modules/wireguard/library.json`). The WireGuard library itself is not declared.
- **Cost notes:**
  - One file-scope `WireGuard` object (`:31-39`).
  - The backend is described as a static-allocation fork with `wireguard_device` in BSS (`modules/wireguard/include/WireguardModule.h:19-23`). That claim was not verified here.
  - Heap snapshots are logged around `up()` (`:379-383, 445-453`). Not measured.
- **Caveats:**
  - Hard-coded network parameters: /24 subnet, gateway `x.x.x.1`, MTU 1420, keepalive 25 s (`:412-435`).
  - `lastHandshakeAgeSec` counts from the `up()` call only. The "optimistic; refined by lib" note is never followed by any refinement (`:457, 312-317`).
  - `isUp()` is just `is_initialized`, not the handshake freshness the interface documents (`modules/wireguard/include/IWireguardProvider.h:37-41`; `modules/wireguard/src/WireguardService.cpp:273-280`).
  - `up()`/`down()` run on the mothership task while `loop()` polls `g_wg` on loopTask, with no lock (`:273, 386-388, 480-489`).
  - Not in the root `library.json` bundle.

### `ws-diag` — WsDiagModule
- **Purpose:** System → Clients tab listing browser tabs tracked by presence: key prefix, IP, status, current page, ages, page history.
- **Install:** `.install<WsDiagModule>()`, which needs PresenceModule. Preset: listed for `.core()`, stub.
- **Module identity:** id `"wsDiag"`, version `"1.0.0"`, priority 80, requires `{"presence"}` (`modules/ws-diag/include/WsDiagModule.h:11-14`). Hook: `onInstall` only.
- **Public API:** None.
- **Web/REST/WS:** `GET /api/v1/system/sockets/form` (Admin) (`modules/ws-diag/include/WsDiagService.h:11`; `modules/ws-diag/src/WsDiagService.cpp:23-41`). Tab `clients` (order 20, `live=true`) on `system` (`:47-55`).
- **Persistence:** None.
- **library.json deps:** `ESPRack`.
- **Cost notes:** 2048 B `DynamicJsonDocument` per request (`:79`). 1536 B start hint (`:59`).
- **Caveats:**
  - The `_ws` member is unused in `buildForm` (`:62-117`).
  - `tab.live=true`, but the page has no WS channel.
  - The tab key `clients` is served from `.../sockets/form`.

---

### `template` — TemplateModule
- **Purpose:** The copy-and-rename starting point for new modules. It's a
  real, small hardware module: a UART line monitor that counts bytes and
  lines on an injected `HardwareSerial` and shows them live. It isn't meant
  to be installed in production.
- **Install:** `#include <TemplateModule.h>` then
  `.install<TemplateModule>(Serial2)`. The constructor takes a
  `HardwareSerial&` and defaults to `Serial1`. Add `TemplateModule` to
  `lib_deps`.
- **Module identity:** id `"template"`, version `"0.1.0"`, priority 60, no
  `requires_` (`modules/template/include/TemplateModule.h`).
  - Hooks: `onInstall` creates the service and registers the UI.
  - `onBegin` runs `ensureLoaded` and opens the port.
  - `onLoop` re-applies settings, drains the port and pushes live data.
  - `onShutdown` closes the port.
- **Public API:** none beyond the module.
- **Web/REST/WS:**
  - Typed feature `"template"`, Authenticated:
    - `GET /api/v1/template/form`
    - `GET`/`POST /api/v1/template/state`
    - WS channel `template`
  - Tabs: `status` (live) and `settings` (postable; admin-only in the UI).
  - Action `template.resetCounters`.
  - Page endpoint `GET /api/v1/template/lines` returns the last 8 lines,
    newest first.
- **Persistence:** `/config/template.json`, ConfigDelegate id `"template"`,
  2048 B buffer. Keys: `enabled, rx_pin, tx_pin, baud, push_ms`. Factory
  defaults come from `FACTORY_TEMPLATE_*`; a pin of `-1` means the UART's
  default pin.
- **library.json deps:** `ESPRack`.
- **Cost notes:**
  - State holds an 8 × 64 B line ring (about 0.5 KB) and a 64 B line buffer.
  - The WS buffer is 1024 B; the REST buffer is auto-sized.
  - Measured in an isolated copy of the demo on 2026-10-13 (whole firmware,
    template in place of LightControl):

    | Build | Static RAM | Flash |
    |---|---|---|
    | C6 | 80,832 B | 2,495,659 B |
    | S3 N16R8 | 83,980 B | 2,581,778 B |

- **Caveats:**
  - It inherits the framework-level issues: the UI Save path and WS
    authorization (see ROADMAP §0).
  - The template keeps its secret-free state reader and its operator-safe
    action exactly so that it works around those issues.

---

## Cross-module findings (inconsistencies / suspected bugs)

### Security and auth

- **Authenticated users who are not admin can read and write sensitive settings over REST.**
  - The endpoint gate is the feature's `spec.auth`; a tab's `auth` only goes into the manifest (`lib/ESPRack/include/WebFeatureDelegate.h:79, 198`).
  - Secret fields emit their plaintext value (`lib/ESPRack/include/web/Form.h:114`).
  - The state POST requires only `IS_OPERATOR`, which is true for any role except `"reader"` (`lib/ESPRack/include/WebFeatureDelegate.h:100-106`; `lib/ESPRack/include/SecurityManager.h:86-88`). Human non-admins have role `"user"` (`:57`), and a factory `guest`/`guest` user is seeded (`modules/security/src/SecuritySettingsService.cpp:126-129`).
  - Affected features (all `spec.auth` Authenticated):
    - `wifi` (`modules/wifi/src/WiFiSettingsService.cpp:410`): reads and writes STA passwords;
    - `ap` (`modules/ap/src/APSettingsService.cpp:142`): AP password;
    - `mqtt` (`modules/mqtt/src/MqttSettingsService.cpp:208`): broker password, also present in `staRead` (`modules/mqtt/include/MqttSettingsService.h:198`);
    - `ntp` (`modules/ntp/src/NTPSettingsService.cpp:46`).

- **WS state writes have no role check.**
  - `WsManager::processRx` applies any `{"type":"p","ch":X}` frame to channel X (`lib/ESPRack/include/WsManager.h:658-666`). The only gate is `IS_AUTHENTICATED` at upgrade time (`modules/security/include/SecurityModule.h:55-58`). Subscribing is not role-checked either (`lib/ESPRack/include/WsManager.h:647-653`).
  - Channels whose WS updater is the full `update()`:
    - `mqtt` (`modules/mqtt/include/MqttSettingsService.h:226-228`);
    - `ntp` (`modules/ntp/include/NTPSettingsService.h:116-118`);
    - `certManager` (`modules/cert-manager/src/CertManagerService.cpp:322-325`). This one accepts `pki_base_url`, `bootstrap_token` and the PEM fields from any authenticated WS client.
  - Telegram already closed this gap (`modules/telegram/src/TelegramSettings.cpp:173, 206`).

- **Install-order auth hole.**
  - SecurityModule requires `presence`, so presence (priority 7) always installs before it. `ctx.security` is still `NullSecurityManager` at that point (`lib/ESPRack/src/Builder.cpp:152, 175`). NullSecurityManager authenticates everyone as admin (`lib/ESPRack/include/NullSecurityManager.h:41-53`).
  - Affected:
    - `/ws/presence` (`modules/presence/include/PresenceModule.h:22`; `modules/presence/src/PresenceService.cpp:84-91`);
    - `/api/v1/ui/dynamic` (priority 6, `modules/ui-dyn/include/UiDynModule.h:18`; `modules/ui-dyn/src/UiDynService.cpp:44-46`).
  - Several comments claim SecurityModule has priority 5 (`lib/ESPRack/include/Module.h:61`, `lib/ESPRack/include/WebManager.h:267`, `lib/ESPRack/src/Builder.cpp:29`). It is 8 (`modules/security/include/SecurityModule.h:36`).

### Builder and core

- **Presets are unimplemented.** `.core()`, `.networking()` and `.full()` are empty stubs (`lib/ESPRack/src/Builder.cpp:28-38`) despite the documentation in `lib/ESPRack/include/Builder.h:59-67`. The documented `.core()` set (security, wifi, ap, system-status, filesystem, ws-diag) omits `presence`, which security and ws-diag both require.

- **Builder knobs are never used.**
  - `bufferSize()`, `cors()` and `wsKeepAlive()` store fields that nothing reads (`lib/ESPRack/include/Builder.h:72-82, 97-100`; grep finds no other references).
  - `App::begin` hard-codes `beginPingPong(20, 60000)` (`lib/ESPRack/src/App.cpp:270`).

- **Build validation does not fail fast.**
  - `lib/ESPRack/include/Builder.h:86-90` says validation failures abort the boot. `build()` only logs and continues.
  - A missing dependency leaves that module's in-degree above zero, so it is reported as "dependency cycle detected", and all modules fall back to insertion order (`lib/ESPRack/src/Builder.cpp:88-95, 135-142`).

- **`onShutdown` is never exercised.** No module overrides `onShutdown`. No restart, factory-reset, firmware-upload or mothership reboot path calls `App::shutdown`:
  - `modules/restart/include/RestartService.h:27-31`
  - `modules/factory-reset/src/FactoryResetService.cpp:46`
  - `modules/upload-firmware/src/UploadFirmwareService.cpp:136`
  - `modules/mothership/src/MothershipService.cpp:1056, 1359`
  - `modules/config-manager-ui/src/ConfigManagerService.cpp:90-93`

  This contradicts `lib/ESPRack/include/Module.h:29-31`.

### Dependencies, packaging and versions

- **Undeclared module dependencies.**
  - factory-reset and upload-firmware `#include <RestartService.h>` (`modules/factory-reset/include/FactoryResetService.h:14`, `modules/upload-firmware/include/UploadFirmwareService.h:17`) without `requires_ {"restart"}` and without a `library.json` entry.
  - `ap` nests under `"wifi"` without requiring it (`modules/ap/src/APSettingsService.cpp:146`).
  - system-status links actions from restart and factory-reset (`modules/system-status/src/SystemStatus.cpp:269-270`) and couples to `MothershipService.h` through `__has_include` (`:18-23`).

- **External libraries missing from `library.json` files.**
  - `AsyncMqttClient` (`modules/mqtt/include/MqttSettingsService.h:15`) and `UniversalTelegramBot` (`modules/telegram/include/TelegramService.h:15`) are declared only in the root `library.json:49-56`, not in their module manifests.
  - `ESP_SSLClient`, needed by the always-compiled core `PersistentTlsClient` (`lib/ESPRack/include/PersistentTlsClient.h:52`), is declared nowhere: not in `lib/ESPRack/library.json:32-45` and not in the root `library.json:33-57`.
  - The core header `lib/ESPRack/include/IMqttDispatcher.h:4` includes `<AsyncMqttClient.h>`.
  - The WireGuard library is undeclared (`modules/wireguard/src/WireguardService.cpp:24-29`).

- **Root bundle omits four modules.** The root `library.json` single-library bundle leaves cert-manager, discovery, mothership and wireguard out of both `srcFilter` and the `-I` flags (`library.json:59-103`). Git-URL consumers cannot install them.

- **Version drift.**
  - Root `library.json` says 0.1.8 (`library.json:3`). `lib/ESPRack/library.json` says 0.3.0 (`lib/ESPRack/library.json:3`). `ESPRACK_VERSION_STR` is 0.2.5 (`lib/ESPRack/include/Version.h:21-23`).
  - Most module `library.json` files say 0.1.8 while their `describe()` says 1.0.0.
  - Telegram's `library.json` says 2.1.0, `describe()` says 2.0.0 (`modules/telegram/include/TelegramModule.h:13`).
  - Most `library.json` descriptions still say "Phase 1 thin wrapper around legacy service", including telegram's (`modules/telegram/library.json:4`).

- **Naming.**
  - Module id `"cert-manager"` is kebab-case (`modules/cert-manager/include/CertManagerModule.h:20`), against the camelCase convention (`lib/ESPRack/include/Module.h:83-84`). Its config and feature id is `"certManager"`.
  - Class `WireGuardModule` lives in `WireguardModule.h`, next to `WireguardService`.

### Stale mDNS and "mesh" references after `128d400`

- No code still uses "mesh" as a name. The only remaining mention is the intentional disclaimer in `modules/discovery/include/DiscoveryModule.h:7-10`.
- Leftover mDNS mentions in code comments:
  - `lib/ESPRack/include/Features.h:14`: "the Discovery/mDNS pair";
  - `modules/system-status/src/SystemStatus.cpp:240`: "mDNS browse tabs";
  - `modules/discovery/src/DiscoveryService.cpp:219`: "mDNS-TXT version".
- **Contradictory `.local` claims.**
  - `modules/ota/src/OTASettingsService.cpp:111-112` and the `128d400` commit message say lwIP resolves `mothership.local`.
  - `lib/ESPRack/src/PersistentTlsClient.cpp:180-183` says `.local` lookup fails with -54.
  - The mothership default URL is `https://mothership.local:8443` (`modules/mothership/include/MothershipService.h:41`).
- **`modules/discovery/DISCOVERY_PROTOCOL.md` is out of date.**
  - The status section still lists the BSS `PeerRow` table as pending (`:16`), although `afd1150` landed one.
  - The spec'd binary `0xE5` header and the ~48 B packed `PeerRow` (`:62-76, 184-197`) differ from the shipped text format `"E5|2|..."` and the ~104 B row (`modules/discovery/src/DiscoveryService.cpp:31-35, 56-68, 134-144`).
  - It gives the task stack as 3072 B (`:216`); the code uses 4096 (`modules/discovery/src/DiscoveryService.cpp:155-157`).
  - It references files under the deleted `modules/discovery/mdns/` directory (`:327-335`).

### Other stale path references

- `/rest/...`:
  - `modules/web-endpoints/include/WebEndpointsService.h:21`: `/rest/uiManifest`;
  - `modules/security/include/SecuritySettingsService.h:66` and `modules/security/src/SecuritySettingsService.cpp:35`: `/rest/securitySettings`.
- `/uiManifest`: `modules/wireguard/include/WireguardModule.h:26`, `modules/wireguard/include/WireguardService.h:28`.
- "WS path stays explicit (/ws/*)", although features are now multiplexed channels:
  - `modules/mqtt/include/MqttSettingsService.h:58`
  - `modules/ntp/include/NTPSettingsService.h:59`
  - `modules/telegram/include/TelegramService.h:24`
- `lib/ESPRack/include/WsManager.h:5` says the socket is at `"/ws"`, but `WS_MUX_PATH` is `"/ws/app"` (`:51`).

### Dead code

- `MOTHERSHIP_WS_PATH` (`modules/mothership/include/MothershipService.h:33`).
- `HARD_UPDATE_CHECK_ATTEMPTS`, `DEFAULT_DEVICE_NAME "Molokijaclock"`, `DEFAULT_DEVICE_VERSION` (`modules/auto-update/include/AutoUpdateService.h:35, 50, 54`).
- `clipPage()` (`modules/presence/src/PresenceService.cpp:11-14`). `setLogSink` and `setLogEnabled` are never called.
- The ui-dyn registry, which has no possible caller.
- Unused `WiFiClientSecure.h` and `HTTPClient.h` includes (`modules/mothership/src/MothershipService.cpp:8-9`).
- WireGuard's WS reader and updater with no `wsPath` (`modules/wireguard/src/WireguardService.cpp:233-237`). Mothership's `broadcastWs` calls with no `wsPath` (`modules/mothership/src/MothershipService.cpp:248, 362, 509`).
- `WireguardService::loop` returns `CHANGED` on both branches of a ternary (`modules/wireguard/src/WireguardService.cpp:277-278`).

### Cert-manager state machine

- The update handler always calls `refreshRuntimeState()` (`modules/cert-manager/src/CertManagerService.cpp:351-355`). That function only derives NeedsEnrollment, Ready or GrayZone from the stored fields (`:536-569`).
- So these states are overwritten immediately after they are set:
  - Enrolling (`:789-793`);
  - EnrollmentFailed after keygen failure or blacklisting (`:835-849, 917-921`);
  - Renewing (`:1196-1200`);
  - "GrayZone (polling /recover)" (`:1356-1360`).
- `ICertProvider::state()` never reports them.

### Mothership result channel

- Results larger than 1 KB are split into chunks. The ring holds 16 chunks and drops the head while splitting (`modules/mothership/src/MothershipService.cpp:927-941`). Any payload over 16 KB, such as `config.dump` (up to 32 KB, `:1306`), loses its own first chunks and cannot be reassembled. The 255-chunk limit check (`:914`) is moot.
- Up to 3 chunks of 1 KB are copied into a 3072 B `_reqDoc` per check-in (`:666-688`). When it overflows, only a warning is logged (`:694-698`). The header comment still says the chunks "fit comfortably in 6 KB" (`modules/mothership/include/MothershipService.h:349-351`).
- Comments say `_reqDoc` and `_respDoc` live in BSS (`modules/mothership/include/MothershipService.h:313-315`; `modules/mothership/src/MothershipService.cpp:574-578, 778-781`). They are members of a heap-allocated object (`modules/mothership/include/MothershipModule.h:37`).

### Functional bugs in individual modules

- **Discovery id truncation.** `char deviceId[40]` against `strcmp` lookups creates duplicate peer rows when the project name is longer than 17 characters (`modules/discovery/src/DiscoveryService.cpp:64, 232, 240`; `lib/ESPRack/include/DeviceIdentity.h:225-226`).
- **OTA settings.**
  - A partial update resets the port and password to factory defaults (`modules/ota/include/OTASettingsService.h:57-59`).
  - `_arduinoOTA` is deleted and recreated from the WiFi event task while loopTask calls `handle()` (`modules/ota/src/OTASettingsService.cpp:79, 88, 139-141`).
- **Auto-update task.**
  - Pinned to core 1 with no unicore guard (`modules/auto-update/src/AutoUpdateService.cpp:200-201`; compare `modules/telegram/src/TelegramService.cpp:224-231`).
  - `WiFiClient::setTimeout` gets seconds here (`modules/auto-update/src/AutoUpdateService.cpp:316`) but milliseconds in telegram (`modules/telegram/src/TelegramService.cpp:507`). One of the two is wrong for the core version in use.
- **system-status `hw_suffix`** drops the `v` octal-PSRAM suffix that `DeviceIdentity::hwSuffix()` adds and AutoUpdate sends as `flv` (`modules/system-status/src/SystemStatus.cpp:311-315` vs `lib/ESPRack/include/DeviceIdentity.h:312-320`).
- **Concurrency without locks.**
  - Presence's `_clients` map (`modules/presence/src/PresenceService.cpp:134-153`, `modules/presence/include/PresenceModule.h:24-32`).
  - MQTT's inbound `_state` mutation (`modules/mqtt/src/MqttSettingsService.cpp:438-483`).
  - AutoUpdate's state written from its OTA task (`modules/auto-update/src/AutoUpdateService.cpp:221-233`).
  - WireGuard's `g_wg` used across tasks (`modules/wireguard/src/WireguardService.cpp:273, 386-388`).
- **Filesystem.**
  - The boot purge deletes every zero-byte file on flash (`modules/filesystem/src/FileSystemManager.cpp:199`).
  - `adoptMounted()` reports flash as mounted even if mounting failed (`modules/filesystem/include/FileSystemFlash.h:35`).
- **Telegram.**
  - The worker is never started if Telegram is enabled after boot (`modules/telegram/src/TelegramService.cpp:220-235`).
  - Messages longer than about 1 KB are truncated because the body is a 1 KB document (`:514-516`).

### Comments that contradict the code

- **cert-manager**
  - The UI text and comments say PEM and token fields are read-only and edits are discarded (`modules/cert-manager/src/CertManagerService.cpp:293-295`), but `update()` adopts any non-empty value (`:106-126`).
  - The header still describes a "Phase 1.0 SKELETON … hard-coded NeedsEnrollment" (`modules/cert-manager/include/CertManagerService.h:5-18`).
  - The CN is documented as `"device-<MAC>"` (`modules/cert-manager/include/CertManagerService.h:81, 241`; `modules/cert-manager/include/ICertProvider.h:43`). The actual CN is `DeviceIdentity::canonical()` (`modules/cert-manager/src/CertManagerService.cpp:744-746`).
  - The recovery-loop comment says each poll generates a new keypair (`:1375-1379`), but the code reuses `_candidateKeyPem` (`:1468-1474`). Draft "let me reconsider" comments remain (`:1297-1309, 1403-1419`).
- **WireGuard**
  - The internals text says edits are discarded (`modules/wireguard/src/WireguardService.cpp:153-157`), but `update()` accepts any non-empty `priv_key` (`:70-79`).
  - The header says `down()` clears the cached triplet (`modules/wireguard/include/WireguardService.h:19-20`); the code keeps it (`modules/wireguard/src/WireguardService.cpp:470-505`).
  - The cert-manager dependency is justified by "same SecretsVault that cert-manager unlocks" (`modules/wireguard/include/WireguardModule.h:10-12`). SecretsVault is core and keyed off the eFuse MAC (`lib/ESPRack/include/SecretsVault.h:5, 117-130`).
  - Comments name three different backends: ciniml (`modules/wireguard/src/WireguardService.cpp:19-21, 32`), the francescolavra fork (`:392`) and WireGuard-ESPRack (`modules/wireguard/include/WireguardModule.h:19-23`).
- **Mothership**
  - The header says it uses `WiFiClientSecure` with `setReuse` (`modules/mothership/include/MothershipService.h:208-214`); the code uses BearSSL (`modules/mothership/src/MothershipService.cpp:543-552`).
  - Comments mention `interval_min*60` and a 5-minute default (`modules/mothership/include/MothershipService.h:116-120`; `modules/mothership/src/MothershipService.cpp:380, 1237`); the default is `interval_s` = 60.
  - `actionCloseTunnel` says WireGuard installs after mothership (`modules/mothership/src/MothershipService.cpp:1011-1016`); WireGuard has priority 14, mothership 35.
  - A "wait, that's higher" draft comment remains (`modules/mothership/include/MothershipModule.h:6-10`).
- **MQTT:** the comment refers to `_state.lastSeenChangedIdx`, which does not exist (`modules/mqtt/include/MqttSettingsService.h:578`).
- **Telegram**
  - The queue is described as "value-type, no heap allocs" (`modules/telegram/include/TelegramService.h:31`), but it holds pointers to `new`'d messages (`modules/telegram/src/TelegramService.cpp:33`).
  - The settings header mentions a "24KB REST envelope" (`modules/telegram/include/TelegramSettings.h:70`); the buffer is 8192.
- **Filesystem:** the header claims "28 endpoints" and "this call only publishes metadata" (`modules/filesystem/include/FileSystemService.h:43-45`). There are 20 endpoints, and `registerManifest` does bind the handlers (`modules/filesystem/src/FileSystemService.cpp:85-99`).
- **ws-diag:** the header says it lists every WS endpoint and client (`modules/ws-diag/include/WsDiagService.h:16-19`); only the presence table is rendered.
- **ui-dyn:** it calls itself "the React app's source of truth" (`modules/ui-dyn/include/UiDynModule.h:5-6`), but the manifest is `/api/v1/ui/manifest`.
