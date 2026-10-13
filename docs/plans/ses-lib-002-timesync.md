# SES-LIB-002 — TimeSync + NTP bridge + `system.setTime` + location-provider interface

Spec: `esp-rack-ses-light/docs/design/ses-light-design.md` §8 SES-LIB-002 (normative),
A0.3 (002). Base: `feature/ses-light-support` after `33c07e6` (SES-LIB-006 reverted: owner
decision, esp-rack carries framework work only; ESPRackPure moved to esp-rack-ses-light).
`TimeArbiter` comes back with this task as part of the core.

## 1. Motivation

- Nothing tracks whether the wall clock is valid; three places guess with a hard-coded
  `1700000000` (`PersistentTlsClient.cpp`, `CertManagerService.cpp`,
  `SecuritySettingsService.cpp` as `kClockSaneAfter`).
- SNTP overwrites whatever another source set, including an implausible captive-portal
  time.
- `configTzTime()` sets libc `TZ` from the NTP module, outside any lock, and only that
  module knows the zone.
- Location needs a core interface so a `gps` module doesn't depend on a `location`
  module.

## 2. Scope (files)

- `lib/ESPRack/include/time/TimeSync.h`, `lib/ESPRack/src/time/TimeSync.cpp` — the
  singleton over `ESPRack::TimeArbiter`, the SNTP hook, RtcHold, TZ owner, the
  `system.setTime` action.
- `lib/ESPRack/include/time/TimeArbiter.h`, `lib/ESPRack/src/time/TimeArbiter.cpp` — the
  arbitration policy (no Arduino/IDF), with `setPolicy()`; host-tested by
  `test-host/` (CMake + a Unity-compatible shim, any C++17 compiler; MSVC here).
- `lib/ESPRack/include/geo/ILocationProvider.h` — interface only (no implementation in
  this task).
- `App.h/.cpp`: `location()/setLocation()`; `TimeSync::instance().begin()` first in
  `App::begin`, `.loop()` first in `App::loop`; `system.setTime` registered before
  `webManager_.begin()`.
- `modules/ntp`: `registerSource(Ntp)`; `configureNTP()` drives `esp_sntp_*` under the
  TCPIP core lock (no `configTzTime`); TZ only through `TimeSync::setTimezone()`;
  Status-tab fields and the set-time control.
- `clockSane()` replaces the three clock checks; `kClockSaneAfter` is removed.
- Versions: core 0.4.0; ntp 0.2.0, security 0.1.9, cert-manager 0.1.1.
- Docs: ARCHITECTURE (time), module guide (time sources, location), catalog (ntp,
  core), consumer guide (TZ, set time), ROADMAP.

## 3. Public API

As the spec (§8 SES-LIB-002). Additions:
- `TimeSync::queueOperatorTime(int64_t utcMs)` — thread-safe; what `system.setTime`
  calls (the sample is submitted on the next `loop()` with the request's mono stamp).
- `TimeSync::registerManifest(WebManager*)` — mounts `system.setTime` (App calls it).
- `timeSourceName(TimeSource)`, `submitResultName(SubmitResult)` — short ASCII names for
  UI/logs.
- `ESPRACK_TIMESYNC_MAX_LISTENERS` (default 4, `#ifndef`).

## 4. Behaviour (interpretations of the spec, recorded)

- **SNTP routing (A0.3).** The Arduino core does not override the weak
  `sntp_sync_time()` (verified: only `liblwip.a` defines it, weak, on C3/C6/S3 libs of
  pioarduino 54.03.21-2). TimeSync provides the strong definition: lwIP hands it the
  server time *instead of* stepping the clock. The hook stamps `{utcMs, monoMs}` under a
  `portMUX` and returns; `loop()` submits `{Ntp, utcMs, monoMs, 250 ms, 0}` and steps
  the clock only on `Applied`. So an implausible NTP time never reaches the clock. A
  disputed NTP time is still applied (NTP ranks first; the spec leaves the reaction to
  the domain): `disputed()` / `Disputed` report it. Before `begin()` (no App)
  the hook behaves like the default (immediate `settimeofday`).
  `sntp_set_time_sync_notification_cb()` is left untouched (nothing calls it any more:
  the default `sntp_sync_time` was its only caller). `sntp_get_sync_status()` reports
  COMPLETED after an accepted NTP sample.
- **SNTP interval.** lwIP's default update interval (`CONFIG_LWIP_SNTP_UPDATE_DELAY`) is
  3 h, equal to NTP's 3 h freshness: authority would lapse right before every resync.
  The NTP module sets 1 h (`esp_sntp_set_sync_interval`).
- **Events.** Computed in `loop()` by diffing validity / stale / disputed / source
  against the previous tick, plus explicit steps:
  - `Stepped(stepMs)` only when a step happened and the clock was valid before and after;
  - `BecameValid(stepMs informational)` increments `clockEpoch`;
  - `BecameInvalid` when uncertainty growth (or nothing) ends validity;
  - `SourceChanged`, `Stale` (false→true), `Disputed` (false→true), `TzChanged`.
  Listeners run on the loop task only, outside any lock. A listener subscribed in a
  module's `onBegin()` still sees `BecameValid` from a restored hold (first `loop()`).
- **Unexplained steps.** Something else calling `settimeofday()` shows up as a jump of
  `sysUtc − mono` > 1 s that TimeSync didn't cause: logged, emitted as `Stepped(stepMs)`
  with source None when valid; validity is unchanged (only `submit()` makes the clock
  valid).
- **`status().source`** = the arbiter's authority, or (when no source is fresh) the
  source of the last accepted sample (RtcHold after a restored hold).
- **RtcHold.** `RTC_NOINIT_ATTR` record `{'TSH1', ver 1, lastSrc, lastSyncUtcMs,
  lastValidUtcS, accMs, crc32}` (32 B). Written on every accepted sample;
  `lastValidUtcS` refreshed every 10 s while valid. Used at boot only for SW, PANIC,
  INT_WDT, TASK_WDT, WDT, DEEPSLEEP resets with a good CRC and
  `lastValidUtcS ≤ time(nullptr) < lastValidUtcS + 7 d` → `restoreHold()`.
- **`system.setTime`.** Admin action, `POST /api/v1/actions/system.setTime?epoch=<s>
  [&ms=<0..999>]`. Missing/non-numeric `epoch` or an implausible time → 400
  problem+json; otherwise 200 `{"queued":true}` and the next `loop()` submits
  `{Manual, epoch·1000+ms, requestMono, 2000, kTimeFlagOperator}`. No rest.proxy
  (`internalHandler`) entry: not required, and it pulled ~1.5 KB on C3.
- **`submit()` off the loop task** is a programming error: logged, rejected
  (`RejectedOutranked`), state untouched.
- **TZ.** `setTimezone(posix, label)` checks the string's shape (starts with a zone name
  or `<`, contains an offset digit, POSIX TZ alphabet only, < 48 chars), takes a mutex, `setenv("TZ")+tzset()`, bumps
  `tzRevision()` only when the string or label changed; `TzChanged` is emitted on the
  next `loop()`. Default `ESPRACK_DEFAULT_TZ` / `_LABEL` until a module sets one.
- **`clockSane()`** = `isValid() || (valid earlier this boot && time(nullptr)
  plausible) || (registeredMask == 0 && time(nullptr) > minSaneUtc())`. The middle
  term (review finding) keeps JWT expiry enforced when the 10-min uncertainty bound
  lapses: the clock keeps running. JWT `iat/exp`, TLS X.509 time and cert expiry use
  it, and read `time(nullptr)` after the check. `isValid()` also requires a plausible
  system clock (between an accepted sample and its `settimeofday()`).
- **Samples** must be stamped with `TimeSync::monoMs()` (esp_timer, 64-bit). A stamp
  in the future (> 1 s) or older than 60 s is rejected as implausible (a `millis()`
  stamp after its 49.7-day wrap would otherwise step the clock).
- **Outranked** samples (GPS while NTP is fresh) are normal: not counted in
  `rejects`, logged at debug level.
- **Listeners** run on a copy of the slot, so one may unsubscribe itself.
- **RtcHold** is not used after deep sleep (RC slow clock, far beyond 30 ppm).
- **`setPolicy()`** keeps `minSaneUtcS = ESPRACK_MIN_SANE_EPOCH` (one floor).

## 5. UI / persistence / threading

- NTP Status tab, compact (agreed with the SES session instead of six fields):
  `clock` = "Valid, ntp, ±1 s" (+ ", sources disagree"), colored success/warning, and
  `clock_diag` = "last step 12 ms, 0 rejected" (live), plus `set_time`
  (`web::DateTime`) and an action button `a_set_time` → `system.setTime` with
  `withFields("epoch=set_time")`. `set_time` is not
  in the WS snapshot, so a tick doesn't overwrite the user's pick.
- Persistence: RTC_NOINIT only (32 B).
- Threading: arbiter state under a `portMUX` (readers on any task: `isValid`,
  `status`, `clockSane`, `utcMsAtMonoMs`); SNTP hook and `queueOperatorTime` only stash
  under the same `portMUX`; TZ under a static FreeRTOS mutex; listeners only from
  `loop()`.

## 6. Budgets

- Always linked: ≤ 6 KB flash and ≤ 700 B RAM on C3 (light-demo C3, before/after).
- **Measured** (light-demo vs `de3263d`, equal path length; includes the LIB-FIX
  follow-ups built with it): C3 flash +13.6 KB (2 566 194 B), RAM +760 B; C6 flash
  +14.0 KB, RAM +760 B; S3 flash +15.8 KB, RAM +784 B. C3 `huge_app.csv` headroom:
  566 KB. On C3, from the ELF: TimeSync code 7.3 KB + 672 B RAM, TimeArbiter 2.8 KB;
  the rest is the NTP status/SNTP changes, strings and unwind tables.
  **Accepted deviation** (SES session, 2026-10-13: up to +10..15 KB flash and < 1 KB
  RAM on C3): TimeSync is a mandatory core part; the S3 target doesn't feel it.

## 7. Acceptance

1. [build] `grep -rn 1700000000` → 0 hits outside TimeSync; `kClockSaneAfter` gone.
   C3/C6/S3 demo + C3 bundle build, 0 warnings, budgets above.
2. [bench-user] Demo semantics: NTP synced → JWTs get `iat/exp` within one loop of the
   sync; NTP disabled (no source) → no `exp`, as today; soft reset → RtcHold keeps `exp`
   checks working; TLS cert-time checks unchanged after sync.
3. [bench-user] NTP → `source == Ntp` within 30 s of getting an IP.
4. [build] WiFi reconnect re-runs SNTP through the same path: `onStationModeGotIP` →
   `configureNTP()` → `esp_sntp_set_sync_interval(1 h)` before every `esp_sntp_init()`;
   [bench-user] after a reconnect the next sync arrives within the 1 h interval.
5. [host] TimeArbiter matrix (18 cases) in `test-host/`.

## 8. Commit plan and rollback

1. `docs(plans)`: this plan.
2. `feat(core)`: TimeArbiter + host test, TimeSync (+ SNTP hook, RtcHold, TZ,
   `system.setTime`), `ILocationProvider`, App wiring, core 0.4.0.
3. `feat(ntp)`: esp_sntp under the core lock, TZ via TimeSync, status fields, set-time.
4. `refactor`: `clockSane()` in PersistentTlsClient, cert-manager, security.
5. `docs`: architecture, guides, catalog, roadmap; measured budgets.

Rollback: revert 5→2. Modules keep working against a core without TimeSync only if 3
and 4 are reverted with it.
