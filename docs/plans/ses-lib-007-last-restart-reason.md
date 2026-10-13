# SES-LIB-007 — Restart reason across reboot

Status: **agreed** with the SES firmware session (2026-10-13; answers below). Spec: `esp-rack-ses-light`
`docs/design/ses-light-design.md` §8 SES-LIB-007, reduced (LIB-FIX-006 covers
`restartDevice()` and `onShutdown()`). The owner approved this task in the core queue.

## 1. API

```cpp
const char* ESPRack::lastRestartReason();   // why this boot happened; stable for the boot
// Built-in reasons (App.h), passed to restartDevice() by the framework modules:
kRestartUser = "user"                  // system.restart (RestartService)
kRestartUpload = "upload"              // firmware upload
kRestartConfigRestore = "config-restore"  // config-manager-ui / mothership restore
kRestartFactoryReset = "system.factoryReset"  // unchanged
kRestartAutoUpdate = "auto-update"
kRestartMothership = "mothership"      // mothership reboot command
kRestartOta = "ota"                    // ArduinoOTA (now through restartDevice)
kRestartHwRestore = "hw-restore"       // System -> Hardware "Restore factory pins"
```

## 2. Behaviour

- **The record.** `restartDevice()` writes
  `RTC_NOINIT {magic, reason[24], crc}` (32 B) right after it claims the restart,
  before the hooks run, so a hook that hangs or panics doesn't lose it. A consumer's own
  reason (≤ 23 chars) is stored as it is.
- **At boot** (App ctor, then cached), `esp_reset_reason()` is combined with the record:
  - **SW with a valid record** → the record's reason.
  - **SW without a record** → `"idf"`, i.e. the safety net: an `esp_restart()` that
    bypassed `restartDevice()`.
  - The other reset reasons map as follows:

    | `esp_reset_reason()` | Value |
    |---|---|
    | POWERON | `power-on` |
    | EXT | `ext` |
    | PANIC | `panic` |
    | INT_WDT | `int-wdt` |
    | TASK_WDT | `task-wdt` |
    | WDT | `wdt` |
    | BROWNOUT | `brownout` |
    | DEEPSLEEP | `deep-sleep` |
    | USB | `usb` |
    | JTAG | `jtag` |
    | anything else | `unknown` |

  - The record is cleared once it has been read, so a later non-SW reset never reuses
    it. A panic in a hook after the record was written reports `panic` (the hardware
    reason wins).
- **Call sites** move to the constants above:
  - restart, upload-firmware, config-manager-ui, auto-update (both paths), mothership
    (reboot → `mothership`, config restore → `config-restore`), and HwRegistry.
  - **OTA:** `ArduinoOTA.setRebootOnSuccess(false)`, and `onEnd` calls
    `restartDevice(kRestartOta)`. The modules' `onShutdown()` then runs on the loop task
    instead of through the IDF safety net.
  - `restartReason()` values change accordingly. Only `kRestartFactoryReset` is compared
    in-tree, and it is unchanged.
- **SystemStatus:** the Identity tab shows "Last restart: <reason>".
- **module-guide note:** `onShutdown()` normally runs on loopTask. In the 3 s fallback,
  or from the IDF safety net (an `esp_restart()` outside `restartDevice`), it runs on
  another task, so a hook must not touch loop-owned state without a lock.

## 3. Budgets and acceptance

- Budget ≤ 1 KB flash, 32 B RTC.
- [build] C3/C6/S3 + bundle.
- [bench-user] The correct value for user, upload, factory-reset, task-wdt and power-on.

## 4. Decisions (SES session, 2026-10-13)

- **Q1:** normalise. The constant **names** stay stable. SES compares with
  `strcmp(reason, ESPRack::kRestartFactoryReset)`, and that constant is unchanged.
  No in-tree code compares reason literals.
- **Q2:** a mothership config restore is `config-restore`. The initiator goes to the
  log.
- **Q3:** `hw-restore`, `wdt`, `deep-sleep`, `usb` and `jtag` are added.
- **SES requirements:**
  - brownout/panic/int-wdt/task-wdt must be told apart reliably: any non-SW reset
    ignores the record, even a valid one.
  - The record is a separate `RTC_NOINIT_ATTR` variable, not a fixed-address section,
    so it can't overlap SES's own RTC variables (the linker places each one).
- **In-progress reason:** the IDF safety net's `restartReason()` is now `"idf"` (it was
  `"esp_restart"`), the same value the next boot reports.

## 5. Implementation notes

- **Measured** (C3 light-demo):
  - +1,152 B flash (152 B over the 1 KB budget: the reason table, the record, the
    Resources row, the OTA and auto-update changes);
  - +32 B static RAM (the cached reason) and the 32 B RTC record;
  - C3 2,606,254; C6 2,607,417; S3 2,702,998; C3 bundle 2,600,442; 0 warnings.
