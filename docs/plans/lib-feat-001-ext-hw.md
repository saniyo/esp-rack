# LIB-FEAT-001 — External HW module instances, resource claims, board pin profile

Status: **implemented** (core 0.5.0, see "Implementation notes"). Agreed
with the SES firmware session (answers Q1–Q6, additions A1–A5) and with the
owner (2026-10-13). External HW modules live in `esp-rack-modules`.

## 1. Motivation

External hardware modules (`esp-rack-modules`: energy meter, digital I/O, RTC, GPS,
1-Wire, …) are installed as **instances**: the same driver may run twice on different
ports. The framework lacks:
1. instances with an id;
2. bookkeeping of the hardware each instance uses, and conflict detection;
3. knowledge of the board's pins that must not be touched (flash, PSRAM, USB, console;
   strapping pins with care). Today a module can be reconfigured from the UI onto a
   PSRAM or USB pin and brick the device until a factory reset;
4. a way to find an instance by interface without adding a typed accessor to `App` for
   every module type.

The core still knows no concrete driver.

## 2. Scope (files)

- `lib/ESPRack/include/hw/ExtHwModule.h` — base class for external HW modules.
- `lib/ESPRack/include/hw/HwResource.h` — `HwResource`, `HwDescriptor` (fixed capacity),
  `HwCheck`. Arduino-free.
- `lib/ESPRack/include/hw/HwClaims.h`, `src/hw/HwClaims.cpp` — the conflict logic over
  plain data (claims + reserved pins). Arduino-free, host-tested in `test-host/`.
- `lib/ESPRack/include/hw/BoardProfile.h`, `src/hw/BoardProfile.cpp` — reserved pins per
  target (C3, C6, S3; plain ESP32 if cheap) from `sdkconfig` macros and runtime facts.
- `lib/ESPRack/include/hw/HwRegistry.h`, `src/hw/HwRegistry.cpp` — registry on the App
  (`app->hw()`): board profile + consumer reservations + live claims, `validate()`,
  `find<I>()`, `forEach<I>()`, System → Hardware tab.
- `Builder.h/.cpp`: `installExtHw<M>(instanceId, args...)`, `reserveGpio(pin, reason)`.
- `App.h/.cpp`: owns `HwRegistry`, runs its boot pass after `onInstall` and before
  `onBegin`.
- `modules/template-ext-hw/` — example external module (Q4: `modules/template` stays as
  is), used twice-instanced by the [build] probe.
- `test-host/test/test_hw_claims/` — [host] acceptance.
- Docs: ARCHITECTURE (hardware section), module guide (writing an external HW module),
  consumer guide (`installExtHw`, `reserveGpio`), catalog note.

## 3. Public API (proposal)

```cpp
namespace ESPRack {

enum class HwKind : uint8_t { Gpio, Uart, I2cBus, I2cAddr, SpiBus, SpiCs, RmtTx, RmtRx,
                              Adc, Ledc, PcntUnit, Other };   // Other: escape hatch, see role
constexpr uint8_t kHwShared   = 0x01;  // may be claimed by several instances (I2C/SPI bus pins)
constexpr uint8_t kHwOptional = 0x02;  // instance works without it (e.g. an LED)
constexpr uint8_t kHwOutput   = 0x04;  // drives the pin (refused on input-only pins)
constexpr int16_t kHwNotConfigured = -1;
constexpr int16_t kHwAnyChannel    = -2;  // RmtTx/RmtRx/Ledc/PcntUnit: allocated at run time

struct HwResource {
  HwKind kind;
  int16_t index;      // GPIO number, UART port, I2C address, channel...; -1 not configured,
                      // -2 any channel (counted against the chip's budget)
  int8_t bus;         // I2C/SPI bus number for I2cAddr/SpiCs; -1 = n/a
  uint8_t flags;
  const char* role;   // static literal: "tx", "rx", "sda", "pps", "relay"...
};

struct HwDescriptor {                 // fixed capacity, no heap
  static constexpr uint8_t kMax = 12;
  HwResource res[kMax];
  uint8_t count = 0;
  bool add(HwKind k, int16_t index, const char* role, uint8_t flags = 0, int8_t bus = -1);
};

struct HwCheck {
  bool ok;
  bool warning;       // ok, but see reason (strapping pin, ADC2 with Wi-Fi)
  char reason[96];    // "GPIO17 used by pzem1 (uart tx)" / "GPIO35 reserved: octal PSRAM"
};

enum class HwState : uint8_t { Ok, NotConfigured, Conflict, Disabled };

class ExtHwModule : public Module {
 public:
  explicit ExtHwModule(const char* instanceId);   // copied (<= 23 chars, [a-z0-9_-])
  const char* instanceId() const;
  // Resources this instance uses NOW (from its settings; -1 = not configured).
  virtual void describeHw(HwDescriptor& d) const = 0;
  // The implemented interface or nullptr; ifaceId is the interface's static
  // string (e.g. IEnergyMeter::kIfaceId). No RTTI.
  virtual void* queryInterface(const char* ifaceId) { (void)ifaceId; return nullptr; }
  virtual const char* typeName() const = 0;        // "pzem004t"; the tab shows typeName@version
  // Optional recovery: put the instance back on its factory pins (and save).
  // System -> Hardware "Restore factory pins" calls it for every instance.
  virtual bool restoreFactoryHw() { return false; }
  // describe(): id = instanceId(); version/priority from the module (template helper).
 protected:
  HwRegistry* hw() const;                           // set before onInstall
  HwState hwState() const;                          // Conflict -> don't touch hardware
  // Instance naming convention helpers:
  //   configPath()  -> "/config/<id>.json"   featureId() -> "<id>"
  //   wsChannel()   -> "<id>"                actionId("calibrate") -> "<id>.calibrate"
};

class HwRegistry {
 public:
  // Proposed settings of one instance against the board + every other claim.
  HwCheck validate(const ExtHwModule& who, const HwDescriptor& proposed) const;
  // After an instance applied new settings: re-read its describeHw().
  void update(ExtHwModule& who);
  HwState state(const ExtHwModule& who) const;
  template <class I> I* find(const char* instanceId);        // via queryInterface(I::kIfaceId)
  template <class I, class Fn> void forEach(Fn fn);          // fn(I&, const char* instanceId)
  bool reserved(int gpio, const char** reason = nullptr) const;
};

class Builder {
  template <typename M, typename... Args>
  Builder& installExtHw(const char* instanceId, Args&&... args);  // new M(instanceId, args...)
  // static_assert both ways: install<M>() with an ExtHwModule says "use
  // installExtHw<M>(id, ...)", installExtHw<M>() requires M : ExtHwModule.
  Builder& reserveGpio(int8_t pin, const char* reason);           // e.g. a board's SD_MMC pins
};

HwRegistry* App::hw();
}
```

## 4. Behaviour

- **Board profile** (`BoardProfile`, data + a few runtime checks; reasons are shown in
  the UI):
  - S3: flash 26–32; octal PSRAM (`CONFIG_SPIRAM_MODE_OCT`) adds 33–37; USB D−/D+ 19/20
    when the console or CDC is on USB (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`,
    `ARDUINO_USB_CDC_ON_BOOT`); UART0 43/44 when it is the console.
  - C3: flash 12–17; USB 18/19 (as above); UART0 20/21 console.
  - C6: flash 24–30; USB 12/13; UART0 16/17 console.
  - Consumer additions: `Builder::reserveGpio(pin, reason)`. The framework's own
    pins (SD_MMC from `BOARD_SD_MMC_*`, the console) are reserved the same way.
  - **Hard-reserved** (validate fails): flash, PSRAM, USB D−/D+ when the console/CDC is
    on USB, UART0 when it is the console, consumer `reserveGpio()` pins.
  - **Strapping pins** (S3 0/3/45/46, C3 2/8/9, C6 8/9/15 and MTMS/MTDI 4/5 — to verify):
    allowed with a warning (`HwCheck::warning`), marked in the tab. S3 GPIO45 text: "high
    at reset selects 1.8 V VDD_SPI — bricks N16R8 flash/PSRAM".
  - **ADC2** pins (S3, classic ESP32; C3's ADC2 too — to verify) get a warning for `Adc`
    claims: reads are unreliable while Wi-Fi is active. ADC1: no warning.
  - **Classic ESP32** (if cheap): GPIO34–39 are input-only; an output claim
    (`kHwOutput`) on them is refused.
  - **Channel budgets** (A1): RMT and LEDC channels (and PCNT units) are allocated
    dynamically in IDF 5 / Arduino 3 (`rmt_new_tx_channel`, `ledcAttach`), so a claim
    may say `kHwAnyChannel`; validate() refuses when the chip's budget is exceeded
    (S3: 4 RMT TX + 4 RX, 8 LEDC, 4 PCNT; C3: 2 + 2 RMT, 6 LEDC, no PCNT; C6: 2 + 2 RMT,
    6 LEDC, 4 PCNT — all to verify). A fixed index (>= 0) still works where it matters.
  - The pin numbers above are from memory of the Espressif datasheets; implementation
    step 2 checks each against the datasheet / IDF `soc/*_pins.h` before coding them, and
    the [host] tests pin the verified tables.
- **Claims**: at boot, after every `onInstall` and before `onBegin`, the registry reads
  `describeHw()` of every `ExtHwModule` in install order. The first claimer of a
  resource wins; a later instance whose required resource collides (or is reserved)
  gets `HwState::Conflict` (log_e, shown in the UI) and must not touch hardware in
  `onBegin`/`onLoop` (it may retry with its factory pins and call `update()`). An
  instance with any required resource at −1 is `NotConfigured`.
- **Sharing**: `kHwShared` resources (an I2C bus's SDA/SCL, an SPI bus) may be claimed
  by several instances; their `I2cAddr` / `SpiCs` must still be unique per bus.
- **Settings changes**: a module's settings updater calls
  `hw()->validate(*this, proposed)` before applying pins; `!ok` → the updater returns
  `StateUpdateResult::ERROR` (400, reason text) and nothing changes. After applying,
  the module calls `hw()->update(*this)`.
- **Lookup**: `find<IEnergyMeter>("pzem1")` / `forEach<IEnergyMeter>(…)` go through
  `queryInterface(I::kIfaceId)`; interface ids are compared with `strcmp` (header
  literals may be duplicated across translation units). Ids are versioned
  (`"energyMeter.v1"`): an incompatible interface change yields nullptr, not UB.
- **Recovery** (A4): System → Hardware admin action "Restore factory pins (all external
  HW)" calls `restoreFactoryHw()` on every instance that supports it, then restarts.
- **Misuse** (A5): static_asserts in `install<M>()` / `installExtHw<M>()`.
- **Instance ids**: `[a-z0-9_-]`, ≤ 23 chars, unique among all modules (framework
  module ids included). A duplicate or invalid id: `log_e`, the instance is not
  installed (Builder's current non-fail-fast semantics), and the Hardware tab lists it
  as rejected.
- **UI**: System → "Hardware" tab (Authenticated), always present (Q5): Table with
  instance, `typeName@version`, resources (`GPIO17 tx, GPIO18 rx, UART1`), state
  (Ok / NotConfigured / Conflict: reason / Disabled), a second table of reserved and
  caution pins (reason, warning level), channel budget use, and the admin
  "Restore factory pins" action.
- No change for consumers that don't use `installExtHw` (light-demo).

## 5. UI / persistence / threading

- The registry holds no persistent state: claims come from the modules' own settings.
- `validate()` / `update()` may be called from the settings updater (async_tcp task);
  `describeHw()` is called from the loop task at boot and from `update()`; the claim
  table is guarded by a mutex.
- Fixed arrays: claims sized from the number of installed instances × `HwDescriptor::kMax`
  (allocated once at boot).

## 6. Budgets

≤ 3 KB flash, ≤ 300 B RAM, plus `HwDescriptor` (≈ 12 × 8 B) per instance. Zero claims
when no `ExtHwModule` is installed (the Hardware tab still lists reserved pins — or is
omitted when there are no instances: Q5).

## 7. Acceptance

1. [host] `HwClaims`: pin conflict; shared I2C bus with different addresses ok, same
   address conflict; SPI CS uniqueness; reserved pins for S3 octal / quad PSRAM, S3 USB
   console on/off, C3, C6; strapping warning; NotConfigured (−1); first-claimer-wins
   order; validate() of a proposal that only moves the instance's own pins.
2. [build] C3/C6/S3 light-demo unchanged (no `installExtHw`); C3 bundle mode;
   `modules/template-ext-hw` built twice-instanced in a probe consumer.
3. [bench-user] two instances of the template on real pins; a UI change onto a reserved
   pin is refused with the reason; a corrupted config with a conflict leaves the
   loser in Conflict and the rest running.

## 8. Commit plan

1. `docs(plans)`: this plan (after agreement).
2. `feat(core)`: HwResource/HwClaims/BoardProfile + host tests.
3. `feat(core)`: ExtHwModule, HwRegistry, Builder::installExtHw/reserveGpio, App wiring,
   Hardware tab.
4. `feat(modules)`: `template-ext-hw`.
5. `docs`.

## Decisions (SES session, 2026-10-13)

- Q1 strapping: allowed with a warning; hard-reserved = flash/PSRAM/USB-console/UART0-console.
- Q2 kinds: Gpio, Uart, I2cBus, I2cAddr, SpiBus, SpiCs, RmtTx, RmtRx, Adc, Ledc, PcntUnit,
  Other (no Touch).
- Q3 framework modules reserve their pins with `reserveGpio()` in `onInstall`.
- Q4 new `modules/template-ext-hw`; `modules/template` unchanged.
- Q5 the Hardware tab is always shown (reserved pins help choosing).
- Q6 `typeName@version` in the tab.
- Additions A1 (channel budgets, `kHwAnyChannel`), A2 (ADC2 + Wi-Fi warning), A3
  (input-only pins, `kHwOutput`), A4 (`restoreFactoryHw` + admin action), A5 (static
  asserts), versioned interface ids — all included above.

## Questions / counterproposals (original)

- **Q1 Strapping pins**: forbid, or allow with a warning (proposed: warn — GPIO0/45/46
  are commonly used after boot; forbidding would break existing boards)?
- **Q2 Kinds**: is `Touch`/`Other` needed now, or keep the enum minimal and extend later
  (the enum value order is ABI for nobody — it isn't persisted)?
- **Q3 Framework modules**: should framework modules that own hardware (filesystem's
  SD_MMC, a status LED) also claim via the registry? Proposal: no new base class for
  them; they `reserveGpio()` in `onInstall` with their reason.
- **Q4 Template**: `modules/template` is the skeleton for every module; turning it into
  an `ExtHwModule` would mislead framework-module authors. Proposal: keep it and add
  `modules/template-ext-hw`. OK?
- **Q5 Hardware tab** when there are no external instances: show reserved pins anyway
  (useful for consumers choosing pins), or omit?
- **Q6 `describe()` for instances**: `id = instanceId`; requires_/priority stay the
  module's. Should the Hardware tab also show the module type's version
  (`typeName()@version`)?

## Implementation notes

What differs from, or adds to, the proposal above:

- **Pin facts verified** against the pioarduino 54.03.21-2 SoC headers and
  enforced by `static_assert` in `BoardProfile.cpp` (flash/MSPI, UART0,
  S3 USB, ADC2, GPIO masks, UART/I2C/SPI/RMT/LEDC/PCNT budgets). S3 flash
  is 27–32; GPIO26 (MSPI CS1) is reserved only with PSRAM. C3/C6 USB pins
  come from the datasheets (no header defines them). Classic ESP32 is
  included: flash 6–11, PSRAM 16/17, input-only 34–39, 16 LEDC channels,
  one shared pool of 8 RMT channels.
- **USB pins** count as in use when USB-Serial-JTAG is a console (the
  prebuilt libs run it as a secondary console) or `ARDUINO_USB_CDC_ON_BOOT`.
  **UART0** is the console unless Serial is USB CDC; then its pins are
  only a caution (ROM boot log). Overrides: `ESPRACK_HW_USB_IN_USE`,
  `ESPRACK_HW_UART0_CONSOLE`.
- **Unknown targets**: no pin table, only conflicts between instances.
- **`HwDescriptor::disabled`**: an instance switched off by its settings
  holds nothing and shows `Disabled`.
- **Reasons for refused writes**: `UpdateError.h`
  (`setUpdateError()`/`takeUpdateError()`, one slot owned by the setting
  task). Feature `POST /state`, `HttpPostEndpoint` and rest.proxy put the
  reason into the 400 problem. Before this change they returned a generic
  text or an empty 400.
- **Registry lock is a leaf**: `describeHw()` is always called outside it.
- **Rejected instances** (invalid id, duplicate id, disabled at build
  time) aren't installed and are listed as "Not installed" in the tab.
  External instances are indexed after framework modules, so an instance id
  can't displace a framework module.
- **The filesystem module** reserves `BOARD_SD_MMC_CLK/CMD/D0` (and D1–D3
  when 4-bit and defined) in `onInstall` (Q3).
- `modules/template-ext-hw` is excluded from the root bundle, like
  `modules/template` (`scripts/check_versions.py`).
- **Instance files** are `/config/hw-<id>.json` (id `hw-<id>`) rather than
  `/config/<id>.json`, so an instance can't share a framework module's
  file (e.g. `cert` and cert-manager's `/config/cert.json`). `system` isn't
  a valid instance id. Builder uses `instanceId()` as the module id
  whatever `describe()` says. WebManager logs duplicate entry ids.
- **Bus pins**: shared pin claims carry the bus number and a role. They're
  shared only on the same bus and in the same role, and a bus can't be put
  on other pins by a second device. I2C addresses with bus −1 are on bus 0.
  Bus numbers are range-checked. On the classic ESP32, fixed RMT TX and RX
  channels with the same number collide.
- **`heldByOther()`**: an instance releases a pin it drove only if no other
  instance claims it now.
- **Measured cost** (pioarduino 54.03.21-2, light-demo, no instances,
  against `feature/ses-light-support` 4fcdd68): C3 flash +15,612 B
  (2,582,790), static RAM +104 B; about 0.45 KB heap for the registry. That
  is over the §6 estimate of 3 KB. The main parts: the Hardware tab
  (≈4.3 KB of code), the claim rules (≈2.5 KB), the registry and the
  Builder/App wiring (≈3 KB), and the reason texts and pin tables
  (≈2 KB). Only the running chip's pin table is compiled. The probe with
  two `template-ext-hw` instances is 2,611,020 B on C3.
