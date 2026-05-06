# ESPRack — Roadmap & Architecture

> **Status**: pre-v0.1 (planning). This document is the source of truth for
> what we're building, in what order, and why. Update it as decisions land.
> Created: 2026-05-06.

---

## 1. Mission

**ESPRack** is a modular, plug-and-play firmware framework for ESP32 (S3, C3,
C6, classic) running Arduino 3.x / IDF 5+. It gives consumer projects:

* A **module contract** — write a class that conforms to a small interface,
  and the framework wires it into:
  - persistent config (file on flash, atomic write, snapshots, AES-encrypted secret fields)
  - a manifest-driven UI (React, dynamic-component primitives)
  - WebSocket live updates
  - lifecycle (init → begin → loop → shutdown)
* An **explicit composition root** (`ESPRack::Builder`) — the consumer's
  `main.cpp` declares which modules are installed in plain readable code.
* A **frontend** that's data-driven for 90 % of fields and pluggable for
  the remaining 10 % (custom React widgets per module).
* **Encryption-at-rest** for password-shaped fields, gated by a build flag.
* **Drop-in extensibility** — adding a new module is one new file plus one
  `.install<MyModule>()` line.

Source project: extracted from `ESP-SES-LightService` (branch
`feature/security-dynamic`). The initial ESPRack release rolls every
generic concern from that codebase into the framework, leaving the
hardware-specific `LightStateService` as the canonical example of an
application module built ON TOP of ESPRack.

---

## 2. Two-repo structure

This is **two separate GitHub repositories**, not a mono-repo with
examples inside. Why: forces clean library/consumer separation, proves
the lib_deps consumption story, lets the demo evolve independently of
framework releases.

```
   ┌─────────────────────────────┐         ┌─────────────────────────────┐
   │  esp-rack                   │         │  esp-rack-light-demo        │
   │  (the library)              │         │  (the test/demo consumer)    │
   │                             │         │                              │
   │  • Module API + Builder     │  lib_   │  platformio.ini:             │
   │  • ConfigManager, WebMgr    │  deps   │    lib_deps =                │
   │  • FormBuilder, SecretsVault│ ──────→ │      gh#user/esp-rack@^0.1   │
   │  • All built-in modules     │         │                              │
   │  • UI primitives (ui/)      │         │  src/main.cpp:               │
   │                             │         │    Builder.core()            │
   │  Tagged releases: v0.1, …   │         │      .install<LightControl>()│
   └─────────────────────────────┘         │      .build();               │
                                            │                              │
                                            │  src/LightControlModule.cpp  │
                                            │  src/LightSettings.h         │
                                            │  interface/  (consumer UI)   │
                                            └─────────────────────────────┘

   Filesystem layout:
   d:\01_projects\03_WorkSpaces\PlatformIO\Projects\
      ├ esp-rack\                ← this repo
      └ esp-rack-light-demo\     ← sibling repo (separate GitHub project)
```

### 2.1 `esp-rack/` — library repo (target v1.0)

```
esp-rack/
├── ROADMAP.md                 # this file
├── README.md                  # quick start
├── ARCHITECTURE.md            # module API, lifecycle, plugin contract
├── CHANGELOG.md
├── LICENSE                    # MIT
├── library.json               # PIO library manifest (root level)
│
├── lib/
│   └── ESPRack/               # framework core
│       ├── include/           # PUBLIC headers
│       │   ├── ESPRack.h      # umbrella include
│       │   ├── Module.h       # IModule + Descriptor + Context
│       │   ├── Builder.h      # ESPRack::Builder
│       │   ├── App.h          # ESPRack::App (built artifact)
│       │   ├── ConfigManager.h
│       │   ├── ConfigDelegate.h
│       │   ├── FormBuilder.h
│       │   ├── WebManager.h
│       │   ├── SecretsVault.h
│       │   └── SecurityHash.h
│       ├── src/               # implementation
│       └── internal/          # truly private helpers
│
├── modules/                   # built-in, optional modules
│   ├── wifi/
│   │   ├── library.json
│   │   ├── include/WiFiModule.h
│   │   └── src/WiFiModule.cpp
│   ├── ap/
│   ├── mqtt/
│   ├── ntp/
│   ├── ota/
│   ├── upload-firmware/
│   ├── auto-update/
│   ├── telegram/
│   ├── security/
│   ├── system-status/
│   ├── ws-diag/
│   ├── filesystem/
│   ├── config-manager-ui/
│   └── web-endpoints/
│
├── ui/                        # frontend (later → npm package)
│   ├── package.json           # publishable as @user/esprack-ui
│   ├── tsconfig.json
│   └── src/
│       ├── index.ts           # public exports
│       ├── components/        # DynamicSettings + dynamic-component elements
│       ├── framework/         # built-in framework UI (security, system, fs)
│       ├── api/
│       ├── contexts/
│       └── utils/
│
├── scripts/
│   ├── build_progmem_www.py
│   ├── version-check.py
│   └── release.py
│
└── .github/workflows/
    ├── build.yml              # CI: build esp-rack-light-demo against this
    ├── publish-npm.yml
    └── release.yml
```

### 2.2 `esp-rack-light-demo/` — consumer repo

```
esp-rack-light-demo/
├── README.md
├── LICENSE
├── platformio.ini             # lib_deps = gh#user/esp-rack@^0.1
├── package.json               # npm dep on @user/esprack-ui (Phase 3+)
├── features.ini               # consumer-side build flags (FT_*)
│
├── src/
│   ├── main.cpp               # composition root (Builder)
│   ├── LightControlModule.h   # the application module
│   ├── LightControlModule.cpp
│   └── LightSettings.h        # state struct + readConfig/update/buildForm
│
├── interface/                 # consumer-side React (optional, can be empty
│   ├── package.json           # — falls back to prebuilt UI bundle from
│   ├── tsconfig.json          # esp-rack release artifact)
│   └── src/
│       ├── App.tsx
│       ├── Plugins.tsx        # custom widget map for /lightControl
│       └── LightChart.tsx     # custom React widget if needed
│
├── data/                      # PIO data partition (factory creds, etc.)
│
└── scripts/                   # consumer-specific build helpers
```

### 2.3 Phased layout

We **don't** start with everything. Phased delivery — see §10.

**Phase 0 (this document + skeleton)**: both directories created, both
have `.gitkeep`s, both have ROADMAP/README placeholders, git initialised
in both.

**Phase 1**: `esp-rack/lib/ESPRack/` core + Builder + Module API +
`modules/{wifi,ap,security,filesystem,system-status}` is enough to make
the demo build & run.

**Phase 2**: port remaining modules one at a time (ntp/ota/mqtt/etc.).
The demo gains tabs as modules land in the library.

**Phase 3**: extract `ui/` as npm package. The demo flips from
"prebuilt UI bundle" to "I have my own interface/".

**Phase 4**: CI, release engineering, GitHub workflows.

### Phased layout

We **don't** start with everything. Phased delivery — see §10.

**Phase 0 (this document + skeleton)**: just `ROADMAP.md`, empty `lib/`,
empty `modules/`, the `examples/light-control/` shell.

**Phase 1 (minimum viable framework)**: `lib/ESPRack/` core + Builder +
Module API + `modules/wifi` + `modules/ap` + `modules/security` +
`examples/light-control/` working end-to-end.

**Phase 2**: port remaining modules (mqtt/ntp/ota/telegram/auto-update/etc.)
one at a time.

**Phase 3**: extract `ui/` as npm package; CI; release engineering.

---

## 3. Module API — formal contract

### 3.1 Interface

```cpp
// lib/ESPRack/include/Module.h
namespace ESPRack {

struct ModuleContext {
  AsyncWebServer*     server;
  ConfigManager*      cfgMgr;
  WebManager*         web;
  WsManager*          ws;          // may be nullptr if FT_WEBSOCKET=0
  SecurityManager*    security;    // may be nullptr if FT_SECURITY=0
  PresenceService*    presence;
  FileSystemManager*  fileSystem;
  // … any other framework-owned singletons modules may need to wire into.
};

struct ModuleDescriptor {
  const char*               id;          // unique stable identifier (e.g. "wifi")
  const char*               version;     // "1.0.0", informational
  std::vector<const char*>  requires;    // dependency module ids
  int                       priority;    // 0..100, lower = installed earlier
  bool                      enabled;     // default true; can be flipped by build flag
};

class Module {
public:
  virtual ~Module() = default;

  // Phase A: identify yourself. Called BEFORE any framework wiring.
  // Must be idempotent and side-effect free — Builder may inspect
  // descriptors before deciding install order.
  virtual void describe(ModuleDescriptor& d) = 0;

  // Phase B: register your concerns. Called once, with full framework
  // context available. This is where you call cfg.attach(),
  // web->registerFeature(), addUpdateHandler(), etc. Must NOT touch
  // hardware (WiFi/peripherals not yet up).
  virtual void onInstall(ModuleContext& ctx) = 0;

  // Phase C: post-network init. Called AFTER WiFi.mode(AP_STA) so it's
  // safe to start network sockets. Hardware peripherals (SD, sensors)
  // also safe to init here. Default: no-op.
  virtual void onBegin() {}

  // Phase D: cooperative tick. Called every iteration of the main
  // loop. Must NOT block. Default: no-op.
  virtual void onLoop() {}

  // Phase E: graceful shutdown. Called on factory reset / restart
  // before reboot. Default: no-op.
  virtual void onShutdown() {}
};

}  // namespace ESPRack
```

### 3.2 Why a 5-phase lifecycle

| Phase | Why separate |
|---|---|
| **describe** | Builder needs descriptors to topologically sort dependencies BEFORE constructing modules. Forbidden side effects keep this pure. |
| **onInstall** | Framework hands the context. Modules wire into ConfigManager, WebManager. Hardware NOT yet up — this is a "register intent" step. |
| **onBegin** | After `WiFi.mode(WIFI_AP_STA)` — the moment when sockets, filesystems, peripherals are all guaranteed alive. |
| **onLoop** | Cooperative scheduling. Framework guarantees no module starves another. |
| **onShutdown** | Symmetric to onBegin. Used by FactoryReset / RestartService. |

### 3.3 Dependency resolution

`Builder.build()` performs:

1. Collect all `describe()` outputs.
2. Verify every `requires` id exists in the install list. Fail fast with
   "module X requires Y but Y is not installed".
3. Detect cycles via Kahn's algorithm. Fail fast on cycles.
4. Topologically sort. Within a level (no edges between modules), order
   by `priority` ascending, then alphabetically by id (deterministic).
5. Assign install order; this becomes the `onInstall` and `onBegin`
   call order. `onLoop` calls all in same order each tick.
6. `onShutdown` calls in REVERSE order so dependents tear down first.

### 3.4 Built-in built modules and their dependencies

| Module | Requires | Notes |
|---|---|---|
| `security` | — | Provides SecurityManager*. Must install BEFORE anything that wants auth-protected endpoints. Priority 5. |
| `wifi` | — | STA + scan. Provides "WiFi up" implicit signal via WiFi.isConnected(). Priority 10. |
| `ap` | — | AP mode. Independent of wifi. Priority 10. |
| `system-status` | — | Read-only health metrics. Priority 80. |
| `ws-diag` | — | Read-only WS diagnostics. Priority 80. |
| `filesystem` | — | LittleFS + SD backends. Priority 20. |
| `config-manager-ui` | filesystem | Save/Restore backup tab. Priority 85. |
| `web-endpoints` | — | Manifest registry tab. Priority 85. |
| `ntp` | wifi | needs internet for SNTP. Priority 30. |
| `ota` | wifi | ArduinoOTA. Priority 40. |
| `upload-firmware` | wifi | HTTP upload endpoint. Priority 40. |
| `auto-update` | wifi | poll an update server. Priority 40. |
| `mqtt` | wifi | broker connection. Priority 40. |
| `telegram` | wifi | bot HTTPS. Priority 40. |

Each module is opt-in via `Builder.install<X>()`. Nothing is enabled by
default; consumers compose what they need.

---

## 4. Builder API — formal contract

```cpp
// lib/ESPRack/include/Builder.h
namespace ESPRack {

class Builder {
public:
  explicit Builder(AsyncWebServer* server,
                   const char* deviceName = "ESPRackDevice",
                   const char* deviceVersion = "v0.0.0");

  // Install a module by type. Forwarded args go to the module's ctor.
  template <typename M, typename... Args>
  Builder& install(Args&&... args);

  // Install a preset bundle (sugar over multiple `install` calls).
  // Provided presets:
  //   .core()       — security + wifi + ap + system-status + ws-diag + filesystem
  //   .networking() — core + ntp + ota + upload-firmware + auto-update
  //   .full()       — networking + mqtt + telegram + config-manager-ui + web-endpoints
  Builder& core();
  Builder& networking();
  Builder& full();

  // Tweak framework knobs. Optional; sensible defaults otherwise.
  Builder& bufferSize(size_t bytes);   // default DynamicJsonDocument cap
  Builder& cors(const char* origin);
  Builder& wsKeepAlive(uint32_t pingSec, uint32_t pongMs);

  // Validate, sort, instantiate. Returns the running App.
  std::unique_ptr<App> build();

private:
  // …
};

}  // namespace ESPRack
```

Consumer's `main.cpp`:

```cpp
#include <ESPRack.h>
#include <modules/wifi/WiFiModule.h>
#include <modules/security/SecurityModule.h>
#include <modules/mqtt/MqttModule.h>
#include "LightControlModule.h"

AsyncWebServer server(80);
std::unique_ptr<ESPRack::App> app;

void setup() {
  Serial.begin(115200);
  app = ESPRack::Builder(&server, "MyLightDevice", "v1.0.0")
    .core()                         // wifi + ap + security + status + filesystem + ws-diag
    .install<NTPModule>()
    .install<MqttModule>()
    .install<LightControlModule>(/*pin_red=*/14, /*pin_green=*/27)
    .build();

  app->begin();
}

void loop() { app->loop(); }
```

That's the entire **delta** for adding a custom module: one include, one
`.install<>` line, one class file.

---

## 5. App lifecycle (consumer-visible)

```
ESPRack::App app;

app.begin():
  ┌─────────────────────────────────────────────────────────┐
  │  1. ESPFS.begin(true)  — mount filesystem                │
  │  2. ConfigManager.begin()                                │
  │  3. WiFi.mode(WIFI_AP_STA)  — early lwIP bring-up        │
  │  4. for each module in install order:                    │
  │       module->onBegin()                                  │
  │  5. WebManager.begin()  — mounts /rest/uiManifest        │
  │  6. WsManager.beginPingPong(20, 60000)                   │
  └─────────────────────────────────────────────────────────┘

app.loop() (called from main loop):
  ┌─────────────────────────────────────────────────────────┐
  │  for each module in install order:                       │
  │     module->onLoop()                                     │
  │  WsManager.processAllQueues()                            │
  │  presenceService.purgeStale() (throttled)                │
  └─────────────────────────────────────────────────────────┘

app.shutdown():
  ┌─────────────────────────────────────────────────────────┐
  │  for each module in REVERSE install order:               │
  │     module->onShutdown()                                 │
  │  ConfigManager flushes pending writes                    │
  └─────────────────────────────────────────────────────────┘
```

---

## 6. Frontend strategy

### 6.1 Two consumer tracks

* **Track A — prebuilt UI**: consumer doesn't touch any React. The
  framework's release artifact ships a `WWWData.h` baked from the
  default UI bundle. Consumer's `platformio.ini` references it via PIO's
  `lib_extra_dirs`. UI features depend purely on the manifest the
  backend emits.

* **Track B — custom UI**: consumer has their own `interface/` with
  `package.json` depending on `@user/esprack-ui`. They can:
  - import primitives (`<DynamicSettings>`, `<TextField>`, etc.)
  - register custom React components for their module's tab via a
    small plugin map
  - rebuild their bundle, framework's `build_progmem_www.py` bakes it
    into firmware

### 6.2 Plugin map for custom widgets

```tsx
// consumer's interface/src/Plugins.tsx
import LightChart from '@my/light-control-ui/LightChart';

export const uiPlugins: ESPRackUiPlugin[] = [
  { featureId: 'lightControl', tabKey: 'chart', component: LightChart },
];
```

`<DynamicFeature>` consults `uiPlugins` first — if a plugin matches, render
it; otherwise fall back to generic `DynamicSettings`. Most modules don't
need plugins.

### 6.3 Manifest is the source of truth

The backend's `WebManager::registerFeature(...)` and
`WebManager::addTabToFeature(...)` calls produce JSON in
`/rest/uiManifest`. The frontend renders strictly from this manifest:

* menu items
* routes
* feature shells (regular vs compound)
* tabs inside compound features
* form schemas
* WS paths

This means a backend module shipping its describe() + form schema
**alone** is enough — no React code needs to ship for the UI to render.

---

## 7. Encryption / security carry-over

`SecretsVault.h`, `SecurityHash.h`, the `addSecretField` mechanism, the
declarative `secretKeys` discovery — all carry over verbatim from
`feature/security-dynamic` into `lib/ESPRack/include/`. Public API.

Build-flag toggles (`FT_SECRETS_VAULT`, `FT_SECURITY`) move from
`features.ini` to either:

* `library.json`'s `build.flags` for framework defaults, or
* the consumer's `platformio.ini` `build_flags` for project overrides.

Convention: `-D ESPRACK_<FEATURE>=<0|1>` instead of `-D FT_<FEATURE>` so
the namespace is clear.

---

## 8. LightControl example — the canonical first module

### 8.1 What's `LightControl`

The current `src/LightStateService.{h,cpp}` in ESP-SES-LightService
manages LED state(s) — colors, modes, schedules, manual overrides. It's
the application-specific code the framework was originally built FOR.
Porting it as the FIRST consumer module proves:

* Application modules can use the same `Module` interface as built-ins
* Custom hardware (GPIO pins) is passed via ctor args
* Application UI (charts, custom widgets) integrates via plugin map
* Persistence via ConfigDelegate works for application state

### 8.2 LightControlModule shape (target)

```cpp
// examples/light-control/src/LightControlModule.h
#pragma once
#include <ESPRack/Module.h>
#include <ESPRack/ConfigDelegate.h>
#include "LightSettings.h"

class LightControlModule : public ESPRack::Module {
public:
  LightControlModule(uint8_t pinRed, uint8_t pinGreen, uint8_t pinBlue);

  void describe(ESPRack::ModuleDescriptor& d) override;
  void onInstall(ESPRack::ModuleContext& ctx) override;
  void onBegin() override;
  void onLoop() override;

private:
  uint8_t pin_r_, pin_g_, pin_b_;
  ConfigDelegate<LightSettings> cfg_;
  WebFeatureEntry<LightSettings>* feature_{nullptr};
  unsigned long last_tick_{0};
  // … the runtime state currently in LightStateService
};
```

```cpp
// examples/light-control/src/LightControlModule.cpp
void LightControlModule::describe(ESPRack::ModuleDescriptor& d) {
  d.id       = "lightControl";
  d.version  = "1.0.0";
  d.priority = 70;     // application code, late
  d.requires = {};     // no framework deps
  d.enabled  = true;
}

void LightControlModule::onInstall(ESPRack::ModuleContext& ctx) {
  cfg_.attach(ctx.cfgMgr,
              "lightState",
              "/config/lightState.json",
              4096,
              this,
              LightSettings::readConfig,
              LightSettings::update,
              false /*autoSave*/,
              nullptr /*validator*/,
              LightSettings::buildForm);

  ESPRack::WebFeatureSpec spec;
  spec.id        = "lightControl";
  spec.title     = "Light Control";
  spec.component = "DynamicSettings";
  spec.menu      = { .label = "Light Control", .icon = "LightMode", .order = 100 };
  spec.restRead  = "/rest/lightForm";
  spec.restUpdate= "/rest/lightForm";
  spec.wsPath    = "/ws/lightLive";
  feature_ = ctx.web->registerFeature<LightSettings>(
      std::move(spec), this,
      LightSettings::buildForm, LightSettings::update,
      LightSettings::staRead,   LightSettings::staUpd,
      4096, 4096);
}

void LightControlModule::onBegin() {
  pinMode(pin_r_, OUTPUT);
  pinMode(pin_g_, OUTPUT);
  pinMode(pin_b_, OUTPUT);
  cfg_.ensureLoaded();
  applyState();
}

void LightControlModule::onLoop() {
  if (!feature_ || !feature_->hasSubscribers()) return;
  unsigned long now = millis();
  if (now - last_tick_ < 200) return;
  last_tick_ = now;
  applyState();
  feature_->broadcastWs("tick");
}
```

### 8.3 Migration map: ESP-SES-LightService → ESPRack

| Source (current) | Target (ESPRack) | Notes |
|---|---|---|
| `lib/framework/ConfigManager.{h,cpp}` | `lib/ESPRack/include/ConfigManager.h` + `src/` | unchanged surface |
| `lib/framework/ConfigDelegate.h` | `lib/ESPRack/include/ConfigDelegate.h` | unchanged |
| `lib/framework/FormBuilder.h` | `lib/ESPRack/include/FormBuilder.h` | unchanged |
| `lib/framework/WebManager.{h,cpp}` | `lib/ESPRack/include/WebManager.h` + `src/` | unchanged |
| `lib/framework/SecretsVault.h` | `lib/ESPRack/include/SecretsVault.h` | rename `FT_SECRETS_VAULT` → `ESPRACK_SECRETS_VAULT` |
| `lib/framework/SecurityHash.h` | `lib/ESPRack/include/SecurityHash.h` | unchanged |
| `lib/framework/ESPReact.{h,cpp}` | DELETED — split into `lib/ESPRack/include/Builder.h` + `App.cpp` | composition root replaces monolith |
| `lib/framework/WiFiSettingsService.{h,cpp}` | `modules/wifi/{include,src}/WiFiModule.{h,cpp}` | wraps as `Module` |
| `lib/framework/APSettingsService.{h,cpp}` | `modules/ap/...` | wraps as `Module` |
| `lib/framework/MqttSettingsService.{h,cpp}` | `modules/mqtt/...` | wraps as `Module` |
| `lib/framework/NTPSettingsService.{h,cpp}` | `modules/ntp/...` | wraps as `Module` |
| `lib/framework/OTASettingsService.{h,cpp}` | `modules/ota/...` | wraps as `Module` |
| `lib/framework/UploadFirmwareService.{h,cpp}` | `modules/upload-firmware/...` | |
| `lib/framework/AutoUpdateService.{h,cpp}` | `modules/auto-update/...` | |
| `lib/framework/TelegramService.{h,cpp}` | `modules/telegram/...` | |
| `lib/framework/SecuritySettingsService.{h,cpp}` | `modules/security/...` | bundles auth + JWT + users |
| `lib/framework/SystemStatus.{h,cpp}` | `modules/system-status/...` | |
| `lib/framework/WsDiagService.{h,cpp}` | `modules/ws-diag/...` | |
| `lib/framework/ConfigManagerService.{h,cpp}` | `modules/config-manager-ui/...` | |
| `lib/framework/WebEndpointsService.{h,cpp}` | `modules/web-endpoints/...` | |
| `lib/framework/FileSystem*.{h,cpp}` | `modules/filesystem/...` | |
| `lib/framework/RestartService.{h,cpp}` | absorbed into `App.cpp` (not a module — framework primitive) | |
| `lib/framework/FactoryResetService.{h,cpp}` | absorbed into `App.cpp` | |
| `interface/src/...` | `ui/src/...` | extract to npm package later (Phase 3) |
| `src/LightStateService.{h,cpp}` | `examples/light-control/src/LightControlModule.{h,cpp}` | the demo |
| `src/main.cpp` | `examples/light-control/src/main.cpp` | composition root for the example |

---

## 9. Naming conventions

| Concern | Convention | Example |
|---|---|---|
| Repo / PIO lib name | kebab-case | `esp-rack` |
| C++ namespace | PascalCase | `ESPRack::Module` |
| C++ class | PascalCase | `WiFiModule` |
| Module id (string) | camelCase | `"wifi"`, `"lightControl"` |
| Config file path | camelCase + .json | `/config/wifiSettings.json`, `/config/lightState.json` |
| REST path | camelCase | `/rest/wifiForm`, `/rest/lightForm` |
| WS path | camelCase | `/ws/wifiStatus`, `/ws/lightLive` |
| JSON option keys (FormBuilder) | snake_case OR short alias | `password` → `pwd`, `tz_label` |
| Build flags | UPPER_SNAKE with `ESPRACK_` prefix | `ESPRACK_SECRETS_VAULT`, `ESPRACK_WEBSOCKET` |
| npm package | `@<scope>/esprack-<part>` | `@saniyo/esprack-ui` |

---

## 10. Phased delivery

### Phase 0 — skeleton (today, hours)

**Deliverables**:
* Empty `esp-rack/` directory committed to git locally.
* `ROADMAP.md` (this file).
* `README.md` placeholder.
* Empty `lib/`, `modules/`, `examples/`, `ui/` directories with `.gitkeep`.

**Acceptance**: directory exists, plan is locked in.

### Phase 1 — MVF (Minimum Viable Framework)

**Goal**: one example builds and runs. WiFi works. Security works.
LightControl demonstrates the consumer flow.

**Deliverables**:
* `lib/ESPRack/` — Module API + Builder + ConfigManager + WebManager +
  FormBuilder + SecretsVault + SecurityHash + App.
* `modules/wifi/` — WiFiModule
* `modules/ap/` — APModule
* `modules/security/` — SecurityModule (bundles SecurityManager
  implementation, /security page backend, JWT tab)
* `modules/system-status/` — SystemStatusModule
* `modules/filesystem/` — FileSystemModule
* `examples/light-control/` — minimal main.cpp + LightControlModule +
  LightSettings + (consumer-side interface optional, can use prebuilt UI)
* `library.json`, `platformio.ini` for the example
* Frontend: copy `interface/` from ESP-SES-LightService into `ui/` AS-IS
  (still consumed via PROGMEM_WWW from the example's build); do not yet
  publish as npm package.

**Acceptance**:
* `pio run -e esp32-c6-devkitc-1` in `examples/light-control/` produces
  firmware
* Flash + boot: WiFi connects, /security login works, /lightControl tab
  renders, scan + connect flow works, smart-slot works
* No regressions vs current ESP-SES-LightService features (status, scan,
  primary/secondary networks, JWT in System tab, Save Backup / Restore
  Backup actions)

### Phase 2 — module parity

**Goal**: every service from ESP-SES-LightService is a module in
ESPRack. Networking + extras presets work.

**Deliverables (one PR per module)**:
* `modules/ntp/` (priority 30)
* `modules/ota/` (priority 40)
* `modules/upload-firmware/`
* `modules/auto-update/`
* `modules/mqtt/` (priority 40)
* `modules/telegram/` (priority 40)
* `modules/ws-diag/` (priority 80)
* `modules/config-manager-ui/` (priority 85)
* `modules/web-endpoints/` (priority 85)

**Acceptance**: `Builder.full()` preset stands up everything that
ESP-SES-LightService had, and runs the same demo on a real device.

### Phase 3 — frontend extraction (npm)

**Goal**: `ui/` becomes `@saniyo/esprack-ui` published on GitHub
Packages (or npmjs.com if public). Consumer projects can `npm i` it.

**Deliverables**:
* `ui/package.json` finalised, semver tags
* `ui/src/index.ts` exports primitives + plugin contract
* `examples/light-control/interface/` references `@saniyo/esprack-ui`
* CI workflow: on git tag push → `npm publish ui/`
* `examples/minimal/` showing consumer with custom UI + plugin map

### Phase 4 — release engineering

**Goal**: cut clean releases consumers can pin to.

**Deliverables**:
* `library.json` semver compliance
* `CHANGELOG.md` automation (conventional commits)
* GitHub Actions:
  * build all examples on every push (linux runner with PIO docker)
  * on tag → publish npm package
  * on tag → attach prebuilt `WWWData-v*.h` and `firmware-c6-v*.bin` to
    GitHub Release
* `release.py` script wrapping the steps
* Documentation in `ARCHITECTURE.md` for module authors

### Phase 5 — polish

**Goal**: production-grade ergonomics.

**Deliverables**:
* `modules/template/` — code-skeleton for a new module (cookiecutter-ish)
* Doxygen API docs for `lib/ESPRack/include/`
* Migration guide ESP-React-style projects → ESPRack
* Example: a third module shipped externally, in its own GitHub repo,
  declaring lib_dep on esp-rack — proves third-party module workflow

---

## 11. Testing strategy

* **Unit tests** (host-native, gtest-ish via PlatformIO Test): for
  pure-C++ helpers — ConfigManager.crc32OfJson, SecretsVault round-trip,
  SecurityHash verify, FormBuilder option-string emission, dependency
  resolution algorithm in Builder.
* **Integration tests** (on-device, automated boot + assertions over WS):
  spin up `examples/light-control/` on a CI runner with a wired ESP32-C6,
  scripted assertions (login, save settings, flash factory reset, etc.).
* **Smoke tests** (manual): each module gets a 5-step manual checklist
  in its own README. CI does not gate on these.

---

## 12. Consumer-facing documentation

Three audiences, three docs:

* **README.md** (newcomer): "I want a smart-home device. How do I start?"
  — quick install, minimal example, link to roadmap.
* **ARCHITECTURE.md** (module author): full Module API, lifecycle,
  Builder, manifest schema, plugin map, dependency resolution. The
  authoritative reference.
* **MIGRATION.md** (existing ESP-React-Framework user): mapping table
  and step-by-step "your project → ESPRack" guide.

`ROADMAP.md` is the planning doc — historical, kept as project diary.

---

## 13. Open questions / decisions still pending

| # | Question | Default | When to decide |
|---|---|---|---|
| 1 | npm scope name? `@saniyo/esprack-ui` vs `@esprack/ui`? | `@saniyo/esprack-ui` (personal scope) | Phase 3 |
| 2 | CMake / IDF component support? | NO — Arduino+PIO only for now | Future |
| 3 | esp8266 carry-over? | Drop — Arduino 3.x focused | Decided |
| 4 | License? | MIT (permissive for downstream) | Phase 4 |
| 5 | Where do `data/` partitions / certs live in module ecosystem? | `examples/<x>/data/` per-example | Phase 1 |

---

## 14. Phase 0 deliverables checklist

### 14.1 `esp-rack/` (library repo)

- [x] Directory created
- [x] `ROADMAP.md` (this file)
- [ ] `README.md` placeholder
- [ ] `.gitignore` (PIO, npm, IDE)
- [ ] `LICENSE` (MIT)
- [ ] Scaffold dirs `lib/`, `modules/`, `ui/`, `scripts/` with `.gitkeep`
- [ ] `git init` + first commit
- [ ] (later) push to GitHub as new repo `esp-rack`

### 14.2 `esp-rack-light-demo/` (consumer repo)

- [ ] Directory created (sibling to `esp-rack/`)
- [ ] `README.md` placeholder noting it consumes `esp-rack`
- [ ] `.gitignore`
- [ ] `LICENSE`
- [ ] Scaffold dirs `src/`, `interface/`, `data/` with `.gitkeep`
- [ ] `git init` + first commit
- [ ] (later) push to GitHub as new repo `esp-rack-light-demo`

Once Phase 0 is locked in, Phase 1 starts with extracting the framework
core out of the existing `lib/framework/` in `ESP-SES-LightService` and
porting `LightStateService` into the demo repo as `LightControlModule`.
