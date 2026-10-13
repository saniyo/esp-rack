# SES-LIB-023..026 — Hardware registry follow-ups (from Pzem004tModule)

Status: **agreed and done** (2026-10-13). They come from the first real `ExtHwModule`
(`Pzem004tModule`, esp-rack-modules). Framework improvements to LIB-FEAT-001; the owner
approved them via the SES session.

## SES-LIB-023 — reserve non-GPIO resources

- **API:**
  - `Builder::reserve(HwKind kind, int16_t index, const char* reason, int8_t bus = -1)`;
  - `Builder::reserveUart(uint8_t port, const char* reason)`;
  - `HwRegistry::reserve(...)`, same as above, for framework modules in `onInstall`.
- **How it's stored:**
  - GPIO kinds go to `reserveGpio()`, as today.
  - Every other kind goes to a separate reserved list in `HwClaims`.
    `check()` fails with `"UART2 reserved: <reason>"`.
  - Reserved channels (RMT/LEDC/PCNT) count toward the budget.
- **Hardware tab:** the reserved table becomes "Resource | Use | Why" (`GPIO12`, `UART2`, ...).

## SES-LIB-024 — a claim lives until the hardware is really released

- **Claims get a `releasing` flag** (`HwClaims`, host-tested):
  - `replace(owner, d, applied, now)`: claims of `owner` that `d` no longer holds become
    releasing; the rest are replaced by `d`.
  - `releaseAll(owner, applied, now)`: every active claim of `owner` becomes releasing.
  - `released(owner)`: drops `owner`'s releasing claims.
  - `applied`: the owner was Ok, so it set the hardware up under those claims. If not
    (Conflict or NotConfigured since boot), the claims are dropped at once. Waiting for
    `released()` from an owner that never opened anything would block that hardware for
    good. Claims that are already releasing stay so.
- **Effect on `check()` for other owners:** a releasing claim blocks like an active one
  (unless both sides are sharable), with `"GPIO18 being released by meter (rx)"`. It
  never blocks its own owner, and it still counts toward channel budgets.
- **Registry changes:**
  - In `update()`, Ok uses `replace()`; Conflict, Disabled and NotConfigured use
    `releaseAll()` instead of dropping the claims. `applied` = the state before was Ok.
  - The boot pass has nothing to release.
  - New `HwRegistry::released(const ExtHwModule&)` and an `ExtHwModule::hwReleased()`
    helper. The instance calls it from its loop once it has let go of the old pins/UART.
- **Risk:** an instance that never calls `released()` keeps its old resources blocked.
  - The module guide says so, and the template shows it.
  - The Hardware tab notes "releasing N".
  - After `kHwReleasingWarnMs` (10 s), the state reads `· warn` and the note
    "releasing N for 14 s, no released()". The claims are never dropped by age: only the
    module knows the hardware is let go.

## SES-LIB-025 — runtime fault per instance

- **API:** `HwRegistry::setRuntimeNote(const ExtHwModule&, const char* note,
  HwNoteLevel level = HwNoteLevel::Warn)`, with `nullptr` clearing it, and
  `enum class HwNoteLevel { Info, Warn, Fault }`. Thread-safe and cheap (a copy into
  the instance row).
- **Hardware tab:** the State column becomes `Ok · fault` / `Ok · warn`. The Note column
  shows the registry reason, the runtime note and "releasing N", in that order.
  - The wording comes from `hwStatusText()`, which is pure and host-tested.
  - A cut note never ends in half a UTF-8 character (`hwUtf8Trim`).
- **Instance:** `ExtHwModule::hwNote(const char*, level)` helper.

## SES-LIB-026 — module guide §10.1 (+ template)

- (a) Strings that the instance's form uses (action ids, titles) must exist **before** its
  `ConfigDelegate` member is constructed. `registerConfig` runs the form reader as a
  secret-key probe in that constructor. So declare those buffers before the
  `ConfigDelegate` member and fill them in its initializer, or keep the form reader a
  pure function of the state.
- (b) UART modules must not `new`/`delete` `HardwareSerial`. The Arduino core keeps a
  pointer to the first object started on each UART for good. Map the port to
  `Serial0/1/2` (or take a `HardwareSerial&`).
- (c) `template-ext-hw`:
  - `hwReleased()` after its pins are let go in `applyPins()`;
  - `hwNote()` for a button stuck low at start (Warn), cleared when it reads high.

## Budgets, acceptance, commits

- Budgets: ≈ 1 KB flash core, +64 B per instance (the note).
- [host] `test_hw_claims`, one case per task:
  - `test_reserved_non_gpio` (023): reserved UART / RMT, including the budget;
  - `test_releasing` (024): releasing blocks others with the right message and doesn't
    block the owner; `released()` frees; `replace()` keeps unchanged claims active;
  - `test_releasing_needs_applied` (024): an owner that never was Ok holds nothing;
  - `test_releasing_overdue` (024): the age, across a `millis()` wrap, and the warning;
  - `test_runtime_note_text` (025): state and note wording, UTF-8-safe cuts.
- [build] C3/C6/S3 + bundle + the probe with two template instances.
- Commits:
  1. `feat(core)`: 023 + 024 + 025 with host tests;
  2. `feat(modules)`: template;
  3. `docs`: 026 and the guides.

## Decisions

- **Q1:** one core commit for 023–025, since they touch the same HwClaims/HwRegistry. The
  host tests are separate cases per task, and the commit message lists the three.
- **Q2:** `releaseAll` on Conflict/Disabled/NotConfigured, but only for claims the
  hardware was set up under (the instance was Ok). Releasing longer than 10 s without
  `released()` is a warning in the tab, and the claim stays.
