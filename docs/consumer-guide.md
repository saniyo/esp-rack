# Creating a consumer project

A **consumer** is a device firmware project built on ESPRack. This guide
follows the reference consumer **`esp-rack-light-demo`**: branch
`feature/ws-single-session`, HEAD `ea8ecd3`, built against esp-rack
`feature/ses-light-support`. References are `path:line` relative to the
demo root unless they start with `esp-rack/`.

Related: [`module-guide.md`](module-guide.md) (writing modules),
[`module-catalog.md`](module-catalog.md) (what you can install),
[`../ARCHITECTURE.md`](../ARCHITECTURE.md).

---

## 0. The model in one paragraph

A consumer is an ordinary PlatformIO Arduino project. It sits **next to** an
`esp-rack` clone (`../esp-rack`).

- PlatformIO finds the core and every module through `lib_extra_dirs`.
  `lib_deps` names the ones to link.
- Three `pre:` scripts run on every build:
  1. Work out a hardware **flavor** tag.
  2. Copy the library's React UI into a throw-away `interface/` folder and
     lay your branding overrides on top.
  3. Run npm, which writes a gzip'd PROGMEM header `WWWData.h`.
- `src/main.cpp` is the composition root: one
  `ESPRack::Builder(...).install<...>()...build()` chain plus your own
  application module(s).

---

## 1. Directory layout

| Item | Tracked | Purpose |
|---|---|---|
| `src/` | yes | `main.cpp` (Builder chain) and the app module (`LightControlModule.h`, `LightStateService.{h,cpp}`). |
| `platformio.ini` | yes | Project config (§2). |
| `factory_settings.ini` | yes | `FACTORY_*` defaults: identity, WiFi, AP, users, NTP, OTA, MQTT, JWT (§3). |
| `features.ini` | yes | `FT_*` flags not tied to a module (§3). |
| `scripts/` | yes | `compute_flavor.py`, `sync_ui.py`, `build_interface.py` (§4). |
| `interface_overrides/` | yes | Branding laid over the UI after each sync: `public/app/icon.png`, `public/app/manifest.json`, `public/favicon.ico` (§5). |
| `interface/` | **no** (`.gitignore:22`) | Build artefact: UI copy + `node_modules` + build output. Never edit it. |
| `data/` | `.gitkeep` only | LittleFS image source (`pio run -t uploadfs`). Gets `data/www/` only when `PROGMEM_WWW` is off. |
| `flash-all.ps1` | yes | Windows: build the UI once, then build and flash every connected board in parallel (§7). |
| `CMakeLists.txt`, `.dummy/`, `sdkconfig.defaults` | yes, but **inert** | Leftovers from pioarduino "Hybrid Compile" (§8). Don't copy them. |
| `build/` | no | A CMake cache an IDE produced by configuring the root `CMakeLists.txt`. Junk. |
| `.pio/` | no | PlatformIO output. |

**Placement is hard-coded.** `esp-rack` must be at `../esp-rack`. That path
appears in:
- `lib_extra_dirs` (`platformio.ini:23-25`)
- `scripts/sync_ui.py:89`
- the fallback output path in `esp-rack/ui/config-overrides.js:18`

---

## 2. `platformio.ini`

### 2.1 Common `[env]` (`platformio.ini:17-89`)

```ini
[platformio]
extra_configs = factory_settings.ini
                features.ini
default_envs  = esp32-c6-devkitc-1

[env]
framework       = arduino
lib_extra_dirs  = ../esp-rack/lib
                  ../esp-rack/modules
lib_deps        = ESPRack
                  WiFiModule ... (one line per module you install)
                  bblanchon/ArduinoJson@6.21.6
                  ESP32Async/AsyncTCP @ 3.4.5
                  ESP32Async/ESPAsyncWebServer @ 3.7.10
                  mobizt/ESP_SSLClient@3.1.3     ; always — core's PersistentTlsClient needs it
                  ; + per module: AsyncMqttClient@0.9.0 (mqtt),
                  ;   https://github.com/saniyo/WireGuard-ESPRack.git (wireguard),
                  ;   https://github.com/saniyo/Universal-Arduino-Telegram-Bot (telegram)
lib_compat_mode = strict
lib_ignore      = ESPAsyncTCP
extra_scripts   = pre:scripts/compute_flavor.py
                  pre:scripts/sync_ui.py
                  pre:scripts/build_interface.py     ; this order matters
build_flags     = ${factory_settings.build_flags}
                  ${features.build_flags}
                  -D NO_GLOBAL_ARDUINOOTA            ; OTAModule owns its ArduinoOTAClass
                  -D PROGMEM_WWW                     ; embed the UI in firmware
                  -D CORE_DEBUG_LEVEL=3
                  -Wno-stringop-overread
                  -D STATIC_IN_BUFFER_SIZE=4096      ; ESP_SSLClient (BearSSL) full-duplex buffers
                  -D STATIC_OUT_BUFFER_SIZE=4096
                  -D CONFIG_ASYNC_TCP_STACK_SIZE=6144 ; frees ~10 KB contiguous heap (measured worst 3060 B)
```

- **`lib_deps` names come from each module's `library.json` `"name"`**, not
  from the directory. For example `modules/wireguard` is `WireGuardModule`,
  while its header is `<WireguardModule.h>`. Includes are flat
  (`<NTPModule.h>`), never `<modules/ntp/...>`.
- **Third-party libraries must be listed by hand.** Neither the core nor the
  module `library.json` files declare them. That covers ESP_SSLClient,
  AsyncMqttClient, WireGuard-ESPRack and the Telegram bot.
- **Version pins have reasons:**
  - ArduinoJson stays on 6.x. Its fixed-pool documents are what the
    heap-budget work relies on.
  - AsyncTCP and ESPAsyncWebServer must come from the **same** owner
    (`ESP32Async`). Mixed owners link two AsyncTCPs (commit `7f92fe2`).
- `lib_ldf_mode` is left at the default `chain`.

### 2.2 Board environments

All envs use `platform = https://github.com/pioarduino/platform-espressif32.git#54.03.21-2`
(Arduino-ESP32 3.2.x / IDF 5.4). Each overrides `build_flags` and **must end
with `${env.build_flags}`**, or it loses `PROGMEM_WWW`, every `FACTORY_*`
and every `FT_*`.

| Env | Flash / partitions | PSRAM | Notes |
|---|---|---|---|
| `esp32-c6-devkitc-1` | 16 MB (`board_upload.flash_size`; the board JSON says 8 MB), `default_16MB.csv` | — | `ARDUINO_USB_MODE=0`, `ARDUINO_USB_CDC_ON_BOOT=0` |
| `esp32-s3-devkitc1-n16r8` | 16 MB, `default_16MB.csv` | 8 MB OPI (`qio_opi`) | USB flags; SD-MMC 1-bit pins (`BOARD_SD_MMC_*`) for `FileSystemSd` |
| `esp32-s3-devkitc1-n8r2` | 8 MB, `default_8MB.csv` | 2 MB QSPI | USB flags |
| `esp32-c3-devkitm-1` | 4 MB, **`huge_app.csv`**, `flash_mode = dio` | — | ⚠ `huge_app.csv` has **one** app slot, so OTA/upload/auto-update can't install an image |
| `node32s` | 4 MB, `min_spiffs.csv` | — | ⚠ 1.875 MiB app slots, likely too small for a full build (not verified) |

Measured on 2026-10-13, C6 with the demo's full module set: static RAM
80,824 B (24.7 %), flash image 2,519,703 B (before LIB-FIX; about +38 KB
of flash after it).

Useful build flags for a consumer's `[env]`:

| Flag | Effect |
|---|---|
| `-D ESPRACK_WS_MAX_CLIENTS=<n>` | Concurrent live UI clients (default 3 on C3, 6 elsewhere). |
| `-D ESPRACK_SHUTDOWN_BUDGET_MS=<ms>` | Per-module `onShutdown()` budget before a reboot (default 500). |

> **Build from PowerShell or cmd, not Git Bash/MSYS.** On a fresh toolchain
> install, pioarduino runs ESP-IDF's `idf_tools.py`, which aborts under MSYS
> ("MSys/Mingw is not supported"). The build then fails with
> `'riscv32-esp-elf-g++' is not recognized`.

---

## 3. `factory_settings.ini` and `features.ini`

Both are PlatformIO INI files with one `build_flags` list of `-D` lines.
Strings need escaped quotes (`-D FACTORY_WIFI_SSID=\"\"`). No script reads
them: they become preprocessor defines that every library sees. Every
`FACTORY_*` the library uses has an `#ifndef` fallback in esp-rack, so every
key is optional.

**`factory_settings.ini` key groups:**

| Group | Keys | Consumed by |
|---|---|---|
| Identity | `FACTORY_PROJECT_NAME`, `FACTORY_PROJECT_VERSION` (`vX.Y.Z`), `FACTORY_HW_REVISION` | Builder args in `main.cpp`; `esp-rack/lib/ESPRack/src/FwTags.cpp`; `App.cpp:96-100` (`DeviceIdentity`) |
| WiFi | `FACTORY_WIFI_SSID`, `_PASSWORD`, `_HOSTNAME` | `modules/wifi` |
| AP | `FACTORY_AP_PROVISION_MODE`, `_PASSWORD`, `_CHANNEL`, `_SSID_HIDDEN`, `_MAX_CLIENTS`, `_LOCAL_IP`, `_GATEWAY_IP`, `_SUBNET_MASK` | `modules/ap` |
| Users | `FACTORY_ADMIN_USERNAME/PASSWORD`, `FACTORY_GUEST_USERNAME/PASSWORD` | `modules/security` |
| NTP | `FACTORY_NTP_ENABLED`, `_TIME_ZONE_LABEL`, `_TIME_ZONE_FORMAT`, `_SERVER` | `modules/ntp` |
| OTA | `FACTORY_OTA_PORT`, `_PASSWORD`, `_ENABLED` | `modules/ota` |
| MQTT | `FACTORY_MQTT_ENABLED`, `_HOST`, `_PORT`, `_USERNAME`, `_PASSWORD`, `_KEEP_ALIVE`, `_CLEAN_SESSION`, `_MAX_TOPIC_LENGTH` | `modules/mqtt` |
| JWT | `FACTORY_JWT_SECRET` | `modules/security` |
| Not set by the demo | `FACTORY_DISCOVERY_PSK` (falls back to the placeholder `"esprack-discovery-dev"`, which **must** be changed for real deployments), `FACTORY_MOTHERSHIP_BASE_URL`, `FACTORY_MOTHERSHIP_INTERVAL_S`, `FACTORY_CERT_BOOTSTRAP_TOKEN` | discovery, mothership, cert-manager |

`SettingValue::format()` resolves placeholders on first boot: `#{platform}`,
`#{unique_id}`, `#{device_id}`, `#{device_id_short}`, `#{random}`
(`factory_settings.ini:1-17`). Keep `FACTORY_PROJECT_NAME` short, because
`#{device_id}` must fit the 32-byte SSID.

A module you write adds its own `FACTORY_<MOD>_*` defaults the same way. The
template uses `FACTORY_TEMPLATE_RX_PIN` and similar keys
(`esp-rack/modules/template/include/TemplateService.h`).

**`features.ini`** holds only non-module flags. A module is enabled by its
`.install<>()` line, and removing its `lib_deps` entry also saves flash. The
demo sets `-D FT_PROJECT=1`, which enables the app module body
(`src/LightControlModule.h:37`). Other flags and their defaults are in
`esp-rack/lib/ESPRack/include/Features.h`:

| Flag | Default | Effect |
|---|---|---|
| `FT_SECURITY` | 1 | |
| `FT_SECRETS_VAULT` | 1 | |
| `FT_HEAP_MONITOR` | 0 | debug: heap-drift sampler |
| `FT_LOOP_PROFILE` | 0 | debug: per-module loop profiler |

> ⚠ The demo's `platformio.ini:156-203` **also** contains copies of the
> `[factory_settings]` and `[features]` sections. The `extra_configs` files
> take precedence, so edits to the copies inside `platformio.ini` are
> silently ignored. A new consumer should keep only the external files.

---

## 4. Build scripts (`scripts/`, all `pre:`, run in this order)

1. **`compute_flavor.py`**
   - Reads the board config: MCU, flash size, PSRAM type and size (from the
     `-n<F>r<R>` board id).
   - Adds `-D ESPRACK_HW_FLAVOR_STRING="<chip>-n<flash>[r<psram>][v]"` and
     prints `[flavor] auto-detected: …`.
   - `FwTags.cpp` embeds the string in the binary.
   - It must match `DeviceIdentity::hwSuffix()`. The update server refuses
     mismatches, and the comments warn that a wrong-flavor image panics.
   - If PSRAM is present but its size is unknown, the flavor gets `r?`.
2. **`sync_ui.py`**
   - **Mode.** If `lib_extra_dirs` mentions `esp-rack`, it uses local-dev
     mode with library root `../esp-rack`. Otherwise it uses "released" mode
     with root `.pio/libdeps/<env>/ESPRack`.
   - **Copy.** It copies `esp-rack/ui/{src,public,package.json,config-overrides.js,progmem-generator.js,tsconfig.json}`
     into `interface/`, keeping `node_modules` and `build`, then lays
     `interface_overrides/` over it.
   - **Seeding.** It seeds missing branding files into `interface_overrides/`
     (it never overwrites) and derives `favicon.ico` from `icon.png`.
   - **Output location.** It exports `ESPRACK_INCLUDE_DIR`, so the UI build
     writes `WWWData.h` into `<libroot>/lib/ESPRack/include/`.
   - **Skip.** `ESPRACK_SKIP_UI=1` skips it.
3. **`build_interface.py`**
   - Runs on a plain `pio run` or on `-t upload`. It's skipped for
     `uploadfs`/`buildfs`/`clean`/`monitor` and when `ESPRACK_SKIP_UI` is set.
   - Steps:
     1. `npm install`
     2. `npm run build` (react-app-rewired + `ProgmemGenerator`, which writes
        `WWWData.h`)
     3. Without `PROGMEM_WWW` only: copy and gzip the build into `data/www/`.

For C++-only iterations, skip the npm step (a `WWWData.h` from an earlier
build must exist): `$env:ESPRACK_SKIP_UI=1; pio run -t upload`.

---

## 5. UI: `interface/` and `interface_overrides/`

- The React app's single source of truth is `esp-rack/ui/`. A consumer
  writes no React: every module describes its UI in C++ (the `web::Form` and
  `WebFeature` builders), and the app renders it from
  `GET /api/v1/ui/manifest`.
- Any file under `interface_overrides/` replaces the same path in
  `interface/`, including `src/*.tsx`. Overriding sources forks the UI and
  you stop getting framework UI updates. **Keep overrides to branding**:
  `icon.png`, `manifest.json` (the library default still says
  "ESP8266 React") and `favicon.ico`.
- `WWWData.h` is written **into the esp-rack checkout**
  (`esp-rack/lib/ESPRack/include/WWWData.h`, ignored there). Two consumers
  that share one esp-rack clone overwrite each other's bundle and branding;
  the last one built wins.
- `App::begin()` serves the bundle with three cache tiers
  (`esp-rack/lib/ESPRack/src/App.cpp:283-344`):

| Assets | Cache lifetime |
|---|---|
| hashed `/js`, `/css`, `/fonts` | 1 year |
| images | 1 day |
| everything else | 10 minutes |

- Unknown `/api/*` paths get a problem+json 404. Other GETs get the SPA
  shell.
- npm: Node and npm must be on PlatformIO's `PATH`. The UI ships **no
  lockfile** and the build uses `npm install`, not `npm ci`, so dependency
  ranges resolve fresh on each machine. Pillow in PlatformIO's penv is
  optional; it downsizes the icon.

---

## 6. What not to commit

These are already ignored by the demo: `.pio/`, `interface/`, `build/`,
`.vscode/c_cpp_properties.json`, `.vscode/launch.json`, `WWWData*.h`,
`*.local` and `data/.tmp_uploads/`.

Add these to a new consumer's `.gitignore`:
- `data/www/`
- `.flash-*.code` (`flash-all.ps1` temp files)
- `sdkconfig.*` and `dependencies.lock` (Hybrid Compile output)
- `.claude/`

---

## 7. `flash-all.ps1`

```powershell
.\flash-all.ps1 [-C3Port COMx] [-S3Port COMy] [-Jobs 3] [-MaxParallel 4]
```

1. Kills stale `pio`, `esptool` and compiler processes that hold COM ports.
   ⚠ It kills **every** such process on the machine, including other
   projects' builds and serial monitors.
2. Detects boards. It considers only **CH34x** USB-serial ports
   (`VID_1A86`) and runs `esptool chip_id` on each.
3. Maps each chip to an env through the `$ChipEnv` table:
   - `ESP32-C3` → `esp32-c3-devkitm-1`
   - `ESP32-S3` → `esp32-s3-devkitc1-n16r8`
   - **Every** S3 maps to N16R8, so an N8R2 board would get the wrong
     flavor.
   - Matching is a substring test.
4. **Phase A:** one `pio run` for the first env builds the UI and that
   firmware, with one retry.
5. **Phase B:** builds the other envs in parallel with `ESPRACK_SKIP_UI=1`.
   Each upload starts as soon as its env is built. Failed builds are retried
   once; uploads are not retried.
6. Prints a per-port report. The exit code is 0 only when everything
   succeeded.

For SES hardware, add your board to `$ChipEnv` and make sure each chip maps
to the right flavor.

---

## 8. `CMakeLists.txt`, `.dummy/`, `sdkconfig.defaults`: don't copy them

pioarduino's Hybrid Compile drops these files into a project when an env
uses `custom_sdkconfig`. The demo's `platformio.ini` has **no**
`custom_sdkconfig`, so they're inert:

- `sdkconfig.defaults` is the stock **ESP32-C3** config plus two mbedTLS
  content-length lines. Those lines wouldn't take effect anyway, because
  `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN` is unset (`sdkconfig.defaults:2158`,
  `3611-3612`).
- The comment at `esp-rack/lib/ESPRack/src/App.cpp:154-161` says the TLS
  OOM was fixed "via `custom_sdkconfig`" in this file. That fix is **not
  active**.
- The `platform_packages = platformio/tool-cmake` lines in four envs are
  probably left over from the same experiment.

---

## 9. `src/`: the composition root and the app module

### 9.1 `main.cpp`

```cpp
AsyncWebServer server(80);
std::unique_ptr<ESPRack::App> app;

void setup() {
  Serial.begin(115200);
  app = ESPRack::Builder(&server, FACTORY_PROJECT_NAME, FACTORY_PROJECT_VERSION)
    .install<FeaturesModule>()   .install<UiDynModule>()     .install<PresenceModule>()
    .install<SecurityModule>()   .install<FileSystemModule>()
    .install<WiFiModule>()       .install<APModule>()
    .install<SystemStatusModule>() .install<WsDiagModule>()
    .install<ConfigManagerUiModule>() .install<WebEndpointsModule>()
    .install<NTPModule>()        .install<DiscoveryModule>()
    .install<OTAModule>()        .install<UploadFirmwareModule>() .install<AutoUpdateModule>()
    .install<MqttModule>()       .install<TelegramModule>()
    .install<CertManagerModule>() .install<WireGuardModule>()   .install<MothershipModule>()
    .install<LightControlModule>()
    .install<RestartModule>()    .install<FactoryResetModule>()
    .build();
  app->begin();      // mounts FS, loads config, onBegin()s, mounts web, starts server
}
void loop() { if (app) app->loop(); }
```

(`src/main.cpp:44-109`)

**The order of `.install<>()` calls is irrelevant.** `Builder` sorts by
`requires_` and then by `(priority, id)`. The effective order for the demo
is:

`features → uiDyn → presence → security → ap → wifi → cert-manager → wireguard → filesystem → discovery → ntp → mothership → autoUpdate → mqtt → ota → telegram → uploadFirmware → lightControl → systemStatus → wsDiag → configManagerUi → webEndpoints → factoryReset → restart`

**Dependencies you must respect:**
- `security` and `wsDiag` require `presence`.
- `wireguard` and `mothership` require `wifi` and `cert-manager`.
- Most network modules require `wifi`.

> ⚠ A missing dependency does **not** stop the boot, whatever `Builder.h`
> says. `Builder::build()` logs `requires 'x' but it is not installed`,
> reports a "dependency cycle", and falls back to **insertion order**
> (`esp-rack/lib/ESPRack/src/Builder.cpp:88-95`, `135-142`). Check the boot
> log for `[ESPRack]` errors.

> Don't use `.core()`, `.networking()` or `.full()`. They're empty stubs
> (`esp-rack/lib/ESPRack/src/Builder.cpp:28-38`).

### 9.2 The application module

| File | Role |
|---|---|
| `src/LightControlModule.h` | The `ESPRack::Module` wrapper: id `lightControl`, priority 60. `onInstall` runs under `#if FT_ENABLED(FT_PROJECT)`, takes the optional `app->telegram()` / `app->mqtt()` and constructs the service. `onBegin`/`onLoop` forward to it. |
| `src/LightStateService.h/.cpp` | `LightState` plus the service. A widget test bench: every form field type, three charts, tables, three actions, a `DataApi` series, and Telegram/MQTT subscriptions. |

To write your own app module, start from `esp-rack/modules/template/` and
follow [`module-guide.md`](module-guide.md). An app module can live in the
consumer's `src/`, like the demo's, or as a module library.

---

## 10. Checklist: a new consumer from scratch

1. Create `<parent>/my-device/` next to `<parent>/esp-rack/`, and check out
   the esp-rack branch you build against.
2. Prerequisites: PlatformIO Core, Node.js + npm, PowerShell or cmd (not
   Git Bash) for builds.
3. Copy from the demo:
   - `platformio.ini`, `factory_settings.ini`, `features.ini`
   - `scripts/*.py` (unchanged)
   - `.gitignore` (add the §6 lines)
   - `data/.gitkeep`
   - optionally `flash-all.ps1` and `.vscode/extensions.json`
4. **Don't copy:** `CMakeLists.txt`, `.dummy/`, `sdkconfig.defaults`,
   `build/`, `interface/`, `.pio/`, `README.md`.
5. Edit `platformio.ini`:
   - Delete the duplicated `[factory_settings]` and `[features]` sections.
   - Trim `lib_deps` to the modules you install, plus their third-party
     libraries.
   - Keep only your board envs, and make sure each ends with
     `${env.build_flags}`.
   - Pick a partition table with **two OTA slots** if you install OTA,
     upload-firmware or auto-update.
   - Set `default_envs`.
6. Edit `factory_settings.ini`:
   - Name, version (`vX.Y.Z`), hardware revision.
   - **Change every default password and secret.**
   - Set `FACTORY_DISCOVERY_PSK` if you install discovery.
   - Add your module's `FACTORY_<MOD>_*` pins and defaults.
7. Write `src/main.cpp` with the Builder chain. Add your app module with a
   unique id, priority around 60 and the `FT_PROJECT` gate.
8. Branding: run one build so `sync_ui.py` seeds `interface_overrides/`.
   Then replace `icon.png`, set the `"name"` in `manifest.json`, delete
   `favicon.ico` so it's derived again, rebuild and commit
   `interface_overrides/`.
9. After the first flash, run the security check against the device:
   ```powershell
   python ..\esp-rack\scripts\check_security.py --host <ip> --admin admin:<pwd>
   ```
   Add `--reader-token <jwt>` to cover the reader-role checks.
10. Build and flash:
   ```powershell
   pio run -e <env>
   pio run -e <env> -t upload
   pio device monitor -e <env>
   ```
   Check the boot log for every module being installed (no
   `[ESPRack] ... requires` errors) and for `[boot] post-<id>` heap lines.
11. Check `git status`: `interface/`, `.pio/` and `build/` must not appear.

---

## Appendix: consumer-side issues in the demo

These matter if you copy from the demo.

- Stale comments:
  - `features.ini:4` still mentions mDNS.
  - `src/LightStateService.h:43-47` claims per-feature `/ws/*` paths.
  - `factory_settings.ini:25` and `scripts/build_interface.py:25-28` refer to
    `/rest/uiManifest`; it's now `/api/v1/ui/manifest`.
  - `sync_ui.py:82-84` describes a "released-mode" block in `platformio.ini`
    that was removed.
- **The demo app doesn't persist its settings.**
  - `LightStateService` never calls `_cfg.saveIfChanged()`. There's no
    update handler for it (compare
    `esp-rack/modules/ntp/src/NTPSettingsService.cpp:28-32`).
  - So a UI save changes RAM only, and `/config/lightState.json` keeps its
    defaults.
- `LightState::readSta` clears `mqtt_pending` inside a *reader*
  (`src/LightStateService.cpp:174`). A machine `GET /api/v1/lightState/state`
  therefore steals rows from the WS stream.
- The `#` MQTT sniffer writes `_values[topic]` with no bound
  (`src/LightStateService.cpp:643`). `MAX_INBOX` is unused.
- The README describes an "RGB-light controller" with a constructor
  `(14, 27, 26)`, `.core()`, `ESPRACK_*` flags and files that don't exist.
  The code is a widget test bench.
- Released mode (`lib_deps` = the esp-rack git URL):
  - The root bundle now includes every module and declares the tested
    third-party set (ESP32Async AsyncTCP/ESPAsyncWebServer, ESP_SSLClient,
    …), at version 0.4.0, and the pure helper library `lib/ESPRackPure`
    (LIB-FIX-010, SES-LIB-006). With `lib_extra_dirs = ../esp-rack/lib` the
    core finds `ESPRackPure` there through its `library.json` dependency.
  - It hasn't been build-tested in that mode yet.
  - `scripts/check_versions.py` guards against version drift.
