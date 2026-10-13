# SES-LIB-006 — ESPRackPure (bootstrap) + host test harness

Spec: `esp-rack-ses-light/docs/design/ses-light-design.md` §8 SES-LIB-006 (normative),
A0. Base: `feature/ses-light-support` @ `de3263d`.

## 1. Motivation

Calendar, astro, time windows, counters, limits and time arbitration are generic,
hardware-free logic that SES (and TimeSync, SES-LIB-002) depends on. It must run
unchanged on the target and on a host so it can be unit-tested. MinGW/MSVCRT ignore
POSIX `Mm.w.d` DST rules, so libc TZ can't be used: PosixTz is implemented here.

## 2. Scope (files)

- `lib/ESPRackPure/library.json` — `{"name":"ESPRackPure","version":"0.1.0","frameworks":"*","platforms":"*"}`
- `lib/ESPRackPure/include/ESPRackPure/{Civil,TimeArbiter,PosixTz,LocalCalendar,SunCalc,TimeWindow,CounterTracker,LimitMonitor}.h`
- `lib/ESPRackPure/src/*.cpp`
- `lib/ESPRack/library.json`: dependency `ESPRackPure`
- root `library.json`: `+<lib/ESPRackPure/src/>`, `-Ilib/ESPRackPure/include`
- `scripts/check_versions.py`: covers `lib/*` (bundle entries per lib)
- `test-host/platformio.ini` (spec, `platform = native`, Unity — GCC hosts)
- `test-host/CMakeLists.txt` + `test-host/shim/unity.h` — the same suites built by any
  C++17 compiler (MSVC here); see §7
- `test-host/test/test_{civil,time_arbiter,posixtz,calendar,suncalc,window,counter,limit}/test_main.cpp`
- `test-host/fixtures/{tz_fixture.h,sun_fixture.h}` + generators
  `scripts/gen_tz_fixture.py`, `scripts/gen_sun_fixture.py`
- docs: `ARCHITECTURE.md`, `docs/module-catalog.md` (pure lib section), README, ROADMAP

## 3. Public API

As the spec (§8 SES-LIB-006), namespace `ESPRack`, plus these clarifications:

- `civil::epochFromBuildDate("Mmm dd yyyy")` → UTC seconds of that date 00:00; 0 on
  malformed input.
- `TimeArbiter` keeps per-source state (last accepted mono, accuracy, implied base
  `utcMs − sampleMonoMs`, last offset) — fixed arrays, no heap.
- `PosixTz::parse` accepts `std offset [dst [offset] [,start[/time],end[/time]]]` with
  `<...>` quoted names, `Mm.w.d`, `Jn`, `n` rules and hour offsets in −167..167 (glibc
  range); a DST name without rules uses the POSIX default `M3.2.0,M11.1.0`.

## 4. Behaviour (interpretations of the spec, recorded)

- **Plausible** sample: `minSaneUtcS ≤ utcMs/1000 < 4102444800` (2100-01-01).
- **Eligibility, "the authority is stale"**: `stale(now)` — no NTP or GPS accepted for
  `staleAfterMs`.
- **Accepted** = `Applied` or `Confirmed`; it refreshes the source's freshness, the
  clock's accuracy reference and (for NTP/GPS) staleness.
- **Validity**: `valid` ⇔ an accepted sample (or a restored hold) exists and
  `u = acc + holdDriftPpm·1e-6·(now.mono − acceptedMono) < maxUncertaintyMs`.
- **Big jump** (valid, `|Δ| > bigJumpMs`, `src < Ntp`, not operator): per-source pending
  set keyed by the implied base `utcMs − sampleMonoMs`. A sample agreeing with the
  pending base (±`crossCheckMs`) is added; once `bigJumpConfirm` samples spanning
  ≥ `bigJumpSpreadMs` agree → `Applied` with the latest sample. A disagreeing sample →
  `RejectedUnconfirmed` and the pending set restarts from it. A non-jump sample from
  the source clears its pending set.
- **Dispute**: compares implied bases (robust to clock steps): GPS fresh, NTP sampled,
  `|baseNtp − baseGps| > disputeMs` and `|baseGps − baseExtRtc| ≤ crossCheckMs`.
- **externalSync** (SNTP already stepped the clock): implausible →
  `RejectedImplausible` (NTP freshness not refreshed); otherwise accepted as an NTP sync
  with `offsetMs = observedStepMs`, `stepClock = false`.
- **restoreHold**: RtcHold becomes the accepted reference with
  `acc + holdDriftPpm·(now.sysUtcMs − lastSyncUtcMs)`; it is never authority by freshness.
- **CounterTracker**: the normative decision table and V1–V18, verbatim.
- **LimitMonitor**: `upper` → exceed when `v > limit`, clear when `v < limit − hyst`
  (mirror for lower); dwell `tripMs`/`clearMs` continuous; an invalid sample resets the
  pending dwell and doesn't change the tripped state.
- **SunCalc**: NOAA Solar Calculator equations (Meeus), geometric elevation by default;
  `nextCrossing` = hour-angle guess, ≤ 3 secant steps, 10-min scan + bisection fallback;
  `false` when no crossing inside `horizonS` (polar day/night).

## 5. UI / persistence / threading

None (pure). Reentrant, no globals, no heap, stack only.

## 6. Budgets

- 0 flash when unused (dead-stripped: no static constructors, no globals).
- ≤ 12 KB flash with everything referenced (measured in §7 by a size probe).
- C3 app-partition headroom: measured at the task's build step (demo, huge_app.csv).

**Measured** (pioarduino 54.03.21-2, Arduino; probe = a minimal sketch that calls every
public entry point with volatile inputs; ESPRackPure symbols only):

| | C3 (RISC-V, soft double) | S3 (Xtensa) |
|---|---|---|
| unused (dependency declared, nothing called) | 0 B (identical image) | 0 B |
| everything referenced, ESPRackPure code | 14.7 KB | 10.9 KB |
| of which astro (SunCalc) | ~7.2 KB | ~4.2 KB |
| libm added beyond what the demo firmware already links | ~2.5 KB (atan, atan2, sqrt) | ~2.1 KB |

The 12 KB budget holds on S3 (the SES target) and is exceeded by ~2.7 KB on C3, where
every double operation is a soft-float call. SunCalc already avoids tan/asin/acos/fmod/
ceil (−6.8 KB of libm on C3). A C3 consumer that doesn't call `astro::` pays ~7.5 KB.
Light-demo (no ESPRackPure calls): RAM unchanged on C3/C6/S3; C3 image 2 552 222 B, the
same as a de3263d build from a path of equal length (19 `__FILE__` strings carry the path).

## 7. Acceptance

1. [host] civil round-trip 1970–2100; `epochFromBuildDate`.
2. [host] arbiter matrix (rank × freshness × plausibility × step × big jump × dispute ×
   operator).
3. [host] `sunPosition` ±0.1°, crossings ±60 s vs the NOAA model for 47–52°N /
   22–40°E at solstices and equinoxes; polar → `false`, no hang.
4. [host] PosixTz vs IANA tzdata 2024b (Python zoneinfo; the POSIX strings are the tzfile
   footers, equal to glibc's results for 2024–2040) on 10 000 random instants
   (Kyiv, London, New York, Sydney); gap/fold 2026-03-29 and 2026-10-25.
5. [host] `monthBounds` 2026–2030; `forEvening` endpoint rules.
6. [host] V1–V18; LimitMonitor tables.
7. [build] C3/C6/S3 demo unaffected; bundle + `check_versions.py` cover `lib/*`.

Host runs: `test-host/platformio.ini` (`pio test -e native`, GCC hosts) and
`test-host/CMakeLists.txt` (any C++17 compiler; MSVC on this machine, with a minimal
Unity-compatible shim). Both compile the same `test_*/test_main.cpp`.

## 8. Commit plan and rollback

1. `docs(plans)`: this plan.
2. `feat(pure)`: ESPRackPure skeleton + Civil + host harness + test_civil.
3. `feat(pure)`: TimeArbiter + tests.
4. `feat(pure)`: PosixTz + LocalCalendar + tz fixture + tests.
5. `feat(pure)`: SunCalc + sun fixture + tests.
6. `feat(pure)`: TimeWindow + CounterTracker + LimitMonitor + tests.
7. `build`: ESPRack dependency, root bundle, `check_versions.py` for `lib/*`, version
   0.4.0 (core-touching), docs.

Rollback: revert the series; nothing outside `lib/ESPRackPure`, `test-host` and the
packaging lines depends on it until SES-LIB-002.
