# ESPRack

> Modular firmware framework for ESP32 (S3 / C3 / C6 / classic) running
> Arduino 3.x / IDF 5+. Plug-and-play modules, manifest-driven UI,
> encryption-at-rest for password fields.

**Status**: pre-v0.1 — see [`ROADMAP.md`](ROADMAP.md) for the plan.

## Quick taste

```cpp
#include <ESPRack.h>
#include <modules/wifi/WiFiModule.h>
#include <modules/security/SecurityModule.h>
#include <modules/mqtt/MqttModule.h>
#include "MyAppModule.h"

AsyncWebServer server(80);
std::unique_ptr<ESPRack::App> app;

void setup() {
  Serial.begin(115200);
  app = ESPRack::Builder(&server, "MyDevice", "v1.0.0")
    .core()                              // wifi + ap + security + status + fs
    .install<MqttModule>()
    .install<MyAppModule>(/*pin=*/14)
    .build();

  app->begin();
}

void loop() { app->loop(); }
```

## Reference consumer

See sibling repo **`esp-rack-light-demo`** — a working RGB-light
controller built as a single `LightControlModule` on top of ESPRack.

## Documentation

- [`ROADMAP.md`](ROADMAP.md) — design, phased delivery, status
- `ARCHITECTURE.md` — module API spec (coming in Phase 1)
- `MIGRATION.md` — moving from `esp32-react`/`ESP-React-Framework` style
  monoliths (coming in Phase 4)

## License

MIT — see [`LICENSE`](LICENSE).
