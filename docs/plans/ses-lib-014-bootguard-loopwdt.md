# SES-LIB-014 — BootGuard + LoopWdt (+ ArduinoOTA coexistence)

Status: **agreed** with the SES firmware session (2026-10-13; the decisions below replace §2 where they differ). Spec: `esp-rack-ses-light`
`docs/design/ses-light-design.md` §8 SES-LIB-014. The owner approved this task in the core
queue. The spec depends on SES-LIB-008 (`NvsRecord`), which is **not** in the approved
queue (see Q1).

## 1. API

```cpp
namespace ESPRack {
struct BootInfo { esp_reset_reason_t reason; uint32_t bootCount; uint8_t quickReboots; bool safeMode; };
namespace BootGuard {
  void begin(uint8_t limit = 4, uint32_t windowMs = 600000, uint32_t stableAfterMs = 120000);
  const BootInfo& info();
  void markStable();      // App::loop calls it once uptime reaches stableAfterMs
  void clearSafeMode();   // also admin action system.clearSafeMode
}
namespace LoopWdt {       // the Arduino loop task on the task watchdog (sdkconfig: 5 s, panic)
  void arm(); void pause(); void resume(); bool armed();
}
}
```

Files: `lib/ESPRack/include/system/{BootGuard.h, LoopWdt.h}` and `src/system/`.

## 2. Behaviour

**BootGuard**
- **Record** (Q1): NVS namespace `esprack`, key `boot`, a blob
  `{magic 'BGD1', bootCount, quick, flags(stable, safeMode), lastBootUtcS, crc32}`.
- **`begin()`** runs early in `App::begin()`, before modules start:
  1. Load the record. Missing or bad → a fresh one.
  2. `bootCount++`.
  3. `quick` — the boots in a row that never became stable:
     - if the previous boot reached `markStable()`, `quick = 1`;
     - else `quick++`;
     - but when the wall clock is valid at this point (a soft reset keeps the RtcHold
       time) and `now − lastBootUtcS > windowMs`, the run is old: `quick = 1`.
  4. `quick ≥ limit` → `safeMode = true`.
  5. Store the record with `stable = false`.
- **Why the window works without a clock:** each boot in a run lasted less than
  `stableAfterMs` (120 s), so `limit` of them fit the window (4 × 120 s < 10 min) without
  any clock.
- **`markStable()`:** App::loop calls it automatically at uptime ≥ `stableAfterMs`.
  `quick = 0`, `stable = true`, store once.
- **safeMode is advisory and sticky.** It survives reboots until `clearSafeMode()`, called
  by the consumer or the admin action. A stable run doesn't clear it, because the reason
  for the loops is unknown (Q3). The consumer reads `info().safeMode` and, for example,
  keeps its outputs off.
- **Wear:** at most two NVS writes per boot (begin, markStable).

**LoopWdt**
- **`arm()`:** `enableLoopWDT()` (Arduino adds `loopTask` to the TWDT and feeds it once
  per `loop()`). Opt-in: the consumer calls it, typically after `app->begin()`.
- **`pause()` / `resume()`** nest (a counter): the first pause calls `disableLoopWDT()`,
  and the last resume calls `enableLoopWDT()` if armed.
- **Framework users:**
  - **OTA:** `onStart` → `pause()`, `onError` → `resume()`. `onEnd` restarts through
    `restartDevice`, which pauses anyway.
  - **`restartDevice()`/`finishAndRestart()`** pause before the shutdown hooks:
    N modules × 500 ms of budget can exceed 5 s.
  - **Long work in other built-in modules runs in their own tasks** (auto-update's HTTP
    OTA, the mothership check-in), so nothing else needs a pause. Checked in the code.

**UI**
- **SystemStatus Resources** gets "Boot": `#<bootCount>`, quick reboots `<n>`, safe mode
  yes/no, and LoopWdt armed/paused.
- **Admin action** `system.clearSafeMode` (core, registered like `system.setTime`).

## 3. Budgets and acceptance

- Budget ≤ 3 KB flash, ≤ 200 B RAM (core).
- [build] C3/C6/S3 + bundle.
- [bench-user] 4 resets within 10 min → safeMode.
- [bench-user] A 2 MB espota upload with `arm()` completes.

## 4. Decisions (SES session, 2026-10-13)

- **Q1:** plain NVS in 014: namespace `esprack` (not `ses`), two writes per boot at most
  (begin, markStable), and no periodic writes. It moves to `NvsRecord` when 008 lands.
- **Q2:** **power-on doesn't count.** `quick` counts crash boots only: panic, int-wdt,
  task-wdt, wdt, `idf` and unknown.
  - power-on, ext, deep-sleep, usb, jtag, brownout and every intended `restartDevice`
    reason neither count nor reset the run; only a stable run resets it. (SES has its
    own brownout backoff.)
  - `info().sinceStable[]` counts boots per class since the last stable run, e.g. a
    brownout series.
  - `windowMs` stays in the signature but isn't used.
- **Q3:** safe mode is sticky. It is cleared by `clearSafeMode()`, a **new firmware**
  (different ELF SHA) or a **factory reset** (`lastRestartReason() ==
  kRestartFactoryReset`). A stable run doesn't clear it.
- **Q4:** LoopWdt is opt-in. `pause()`/`resume()` are public for consumers' long one-off
  work.
- **SES reads** `info().safeMode` at start (no lamp, CRIT), plus `quickReboots` and the
  per-class counters.

## 5. Implementation notes

- **Measured** (C3 light-demo against SES-LIB-007): +5,204 B flash, +264 B static RAM.
  - **Core** ≈ 3.6 KB (budget 3 KB): `BootGuard::begin` 932 B (logging),
    `registerManifest` 916 B (the admin action), plus the record, LoopWdt and strings.
  - **Modules** ≈ 1.1 KB: the SystemStatus "Boot" row and the OTA pause/resume.
  - **RAM:** mostly the two static mutexes (BootGuard, LoopWdt), plus the 32 B record
    and BootInfo.
  - Builds: C3 2,611,458; C6 2,612,675; S3 2,709,166; C3 bundle 2,605,516; 0 warnings.
