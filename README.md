# ESPRack

> Modular firmware framework for ESP32 (C3 / C6 / S3 / classic) on
> Arduino-ESP32 3.x / ESP-IDF 5 (pioarduino platform). You install
> plug-in modules, the React UI is generated from a manifest the device
> serves, the device exposes a versioned REST + WebSocket API, and
> secret settings are encrypted on flash.

**Status:** pre-release, under active development. The framework and 23
modules run on hardware in the reference consumer
[`esp-rack-light-demo`](../esp-rack-light-demo). The API is not frozen.
Current state, work in flight and known problems are in
[`ROADMAP.md` → Status](ROADMAP.md#0-status-2026-10-13).

## Quick taste

```cpp
#include <ESPRack.h>
#include <SecurityModule.h>    // headers are flat: each module's include/ is on the path
#include <PresenceModule.h>
#include <FileSystemModule.h>
#include <WiFiModule.h>
#include <APModule.h>
#include <NTPModule.h>
#include "MyAppModule.h"

AsyncWebServer server(80);
std::unique_ptr<ESPRack::App> app;

void setup() {
  Serial.begin(115200);
  app = ESPRack::Builder(&server, FACTORY_PROJECT_NAME, FACTORY_PROJECT_VERSION)
    .install<PresenceModule>()
    .install<SecurityModule>()           // requires "presence"
    .install<FileSystemModule>()
    .install<WiFiModule>()
    .install<APModule>()
    .install<NTPModule>()                // requires "wifi"
    .install<MyAppModule>(Serial2)       // hardware handles are ctor args
    .build();                            // dependency + priority sort
  app->begin();                          // mounts FS, starts modules, starts the web server
}

void loop() { app->loop(); }
```

The order of `.install<>()` calls doesn't matter: `Builder` sorts the
modules by their declared dependencies and priorities. List every module
you want explicitly. The `.core()`/`.networking()`/`.full()` presets are
still empty stubs. Each module is a PlatformIO library, so the consumer
also has to list it in `lib_deps`. See
[`docs/consumer-guide.md`](docs/consumer-guide.md).

## Repository layout

| Path | What |
|---|---|
| `lib/ESPRack/` | Core library (`ESPRack`). Hardware-independent: Module/Builder/App lifecycle, ConfigManager, the web layer (manifest, form DSL, REST, WS), security primitives, provider interfaces. |
| `modules/<name>/` | One PlatformIO library per feature: wifi, ap, security, mqtt, telegram, ntp, ota, discovery, mothership, wireguard, … Full list in [`docs/module-catalog.md`](docs/module-catalog.md). `modules/template/` is the skeleton to copy for new modules. |
| `ui/` | The React UI. A consumer's build copies it in and embeds it into the firmware as `WWWData.h`. |
| `api/` | Generated OpenAPI spec + manifest dump (`scripts/gen_openapi.py`). |
| `scripts/` | Dev tools: OpenAPI generator, mock mothership server, serial-log converter, `check_versions.py` (version/bundle drift), `check_module_deps.py` (cross-module includes vs declared dependencies), `check_security.py` (on-device auth regression). |
| `test-host/` | Host unit tests for the Arduino-free parts of the core (`time/TimeArbiter`): CMake + a Unity-compatible shim, any C++17 compiler; `run-host-tests.ps1` on Windows. |
| `docs/` | Guides (below) and `docs/plans/`, the design notes and audits behind past work. |

## Documentation

| Document | For |
|---|---|
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | How the core works: layers, lifecycle, persistence, web/WS/REST, security, memory. |
| [`docs/module-guide.md`](docs/module-guide.md) | **Writing a module**: hooks, settings, UI forms, live status, actions, endpoints, messaging, hardware rules. |
| [`docs/consumer-guide.md`](docs/consumer-guide.md) | Creating a device firmware project on top of ESPRack (based on `esp-rack-light-demo`). |
| [`docs/module-catalog.md`](docs/module-catalog.md) | Every built-in module: purpose, API, dependencies, cost. |
| [`ROADMAP.md`](ROADMAP.md) | Original design and phased plan, plus the current status section. |
| `docs/plans/*.md` | Design notes per topic: API standardization, discovery protocol, memory/heap work, mothership, WireGuard, tree menu, form DSL. |

## Branches

`master` is **stale**: it's at `8f8a262` (2026-05-09) and 128 commits
behind the integration line. The current integration line is
**`feature/ses-light-support`**. It contains `feature/ws-single-session`,
which in turn contains `feature/api-v1`, `feature/device-mesh`,
`feature/web-*`, `feature/wireguard-*` and `debug/esp32async-migration`.
Build consumers against it. The branch table is in
[`ROADMAP.md` → Status](ROADMAP.md#0-status-2026-10-13).

## License

MIT — see [`LICENSE`](LICENSE).
