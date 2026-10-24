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
[`ROADMAP.md` → Status](ROADMAP.md#0-status-2026-10-15).

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
| `modules/<name>/` | One PlatformIO library per feature: wifi, ap, security, mqtt, telegram, ntp, ota, discovery, mothership, wireguard, … Full list in [`docs/module-catalog.md`](docs/module-catalog.md). `modules/template/` is the skeleton to copy for new modules; `modules/template-ext-hw/` for hardware drivers installed as instances. |
| `ui/` | The React UI. A consumer's build copies it in and embeds it into the firmware as `WWWData.h`. |
| `api/` | Generated OpenAPI spec + manifest dump (`scripts/gen_openapi.py`). |
| `scripts/` | Dev tools: OpenAPI generator, mock mothership server, serial-log converter, `check_versions.py` (version/bundle drift), `check_module_deps.py` (cross-module includes vs declared dependencies), `check_security.py` (on-device auth regression). |
| `test-host/` | Host unit tests for the Arduino-free parts of the core (`time/TimeArbiter`, `hw/HwClaims`, `persist/*`, `power/PowerGuard`, the Telegram command parser): CMake + a Unity-compatible shim, any C++17 compiler; `run-host-tests.ps1` on Windows. |
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

## What's new (0.7.2, 2026-10-24)

- **Live pages recover by themselves:** the device sends a heartbeat and
  drops a client whose queue is stuck; the page reconnects after 15 s
  without frames instead of freezing until a reload.
- **Live forms:** fields on a live settings tab are saved like a Settings
  save (answered, refused values come back); a cleared number field keeps
  the setting instead of becoming the minimum; sliders don't jump back
  mid-drag; charts and tables don't double after a hidden tab returns.
- **Charts:** every Trend series keeps its color and type; series names
  (`line(key, color, type, name)`).
- **Bluetooth:** the radio's real cost is internal RAM; see the consumer
  guide.

## 0.7.1 (2026-10-24)

- **Fixes:** firmware upload from the web page (it got 404), and an empty
  POST to the upload endpoint no longer restarts the device.

## 0.7.0 (2026-10-24)

- **Buses as shared hardware:** a CAN port (`hw/ICanPort.h`) and an I2C bus
  (`hw/II2cBus.h`) are external HW instances that software modules and device
  instances find by id. I2C devices claim only their address, and a conflict
  needs the same address on the same controller. The drivers (`can-twai`,
  `can-mcp2515`, `i2c-bus`) live in esp-rack-modules.
- **Interfaces of plain modules:** `Module::queryInterface()` and
  `app->find<I>("moduleId")`.
- **Bluetooth LE:** `modules/ble` owns the radio (NimBLE-Arduino 2.x) and
  publishes `IBleProvider` (`app->ble()`): scans into a bounded table and
  managed central links (discover, connect, subscribe, reconnect). See the
  `ble` entry in [`docs/module-catalog.md`](docs/module-catalog.md).

## Earlier (2026-10-15)

- **External hardware:** drivers installed as instances
  (`installExtHw<M>("id", ...).owner("module")`) with pin, UART and channel
  claims and reservations, claims held until released, runtime notes, and an
  External Hardware page grouped under the owning module.
- **Persistence and power:** `NvsRecord<T>`, `RtcScratch<T>`,
  `CheckpointPolicy`, and `PowerGuard`, which pauses flash writes on a power
  failure.
- **Time:** clock steps as int64 (`TimeStep`); the time zone is served at
  runtime (`/api/v1/ui/device`).
- **UI:**
  - layout set from the backend (`web::Cols` / `web::Break`, `span`,
    `fullRow`, one column by default) and a 24-hour date/time picker;
  - the manifest is a cached, compact skeleton, with feature actions loaded
    lazily;
  - opt-in static form schema (`staticSchema()`).
- **Messaging:** Telegram bot commands (`onCommand`, allowed chats, admins);
  MQTT device availability with a last will.
- **Build:** each consumer builds its own UI bundle in `<project>/.pio/www/`.

## Branches

- `master` is the published line. On 2026-10-15 it was fast-forwarded to the
  integration branch at `83a8225`.
- `feature/ses-light-support` is the integration branch. Verified work lands
  there first, then on `master`.

Details are in [`ROADMAP.md` → Status](ROADMAP.md#0-status-2026-10-15).

## License

MIT — see [`LICENSE`](LICENSE).
