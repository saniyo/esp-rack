#pragma once
#ifndef ESPRACK_TIME_ARBITER_H
#define ESPRACK_TIME_ARBITER_H

// TimeArbiter — decides which time source may set the clock, and whether the
// clock is valid. The policy behind TimeSync (time/TimeSync.h), kept free of
// Arduino/IDF so it is unit-tested on a PC (test-host/): the caller supplies
// "now" (system UTC + monotonic ms) and applies the verdict (settimeofday).
// No heap, no globals, reentrant.
//
// Rank: Ntp(5) > Gps(4) > ExtRtc(3) > Manual(2) > RtcHold(1) > None(0).
// Authority = the highest-rank source whose last ACCEPTED sync is younger than
// freshMs[src] (Manual / RtcHold never, freshMs = 0).
//
// submit():
//   1. implausible (utc < minSaneUtcS or >= 2100-01-01)   -> RejectedImplausible
//   2. applied = utcMs + (now.mono - sampleMono); delta = applied - sysUtc
//   3. eligible if !valid || src >= authority || stale(now)
//      || (operator flag && src == Manual && authority not Gps/Ntp)
//      otherwise                                              -> RejectedOutranked
//   4. big jump (valid, |delta| > bigJumpMs, src < Ntp, no operator flag):
//      Pending until bigJumpConfirm samples, spread >= bigJumpSpreadMs, agree
//      on the implied base (utc - mono) within crossCheckMs; a disagreeing
//      sample -> RejectedUnconfirmed and restarts the pending set
//   5. |delta| < stepMinMs[src] -> Confirmed (no step), else Applied (step)
// Accepted (Applied/Confirmed) refreshes the source, the clock's accuracy
// reference and, for Ntp/Gps, staleness.
//
// valid(now): an accepted reference exists and
//   u = acc + holdDriftPpm * 1e-6 * age < maxUncertaintyMs.
// stale(now): no Ntp/Gps accepted within staleAfterMs (a WARN, not invalidity).
// disputed(): Ntp's implied base differs from a fresh Gps by > disputeMs while
//   that Gps agrees with ExtRtc within crossCheckMs (2-of-3), re-evaluated on
//   every sample.

#include <stdint.h>

namespace ESPRack {

enum class TimeSource : uint8_t { None = 0, RtcHold = 1, Manual = 2, ExtRtc = 3, Gps = 4, Ntp = 5 };
constexpr uint8_t kTimeSourceCount = 6;

enum class SubmitResult : uint8_t {
  Applied, Confirmed, Pending, RejectedImplausible, RejectedOutranked, RejectedUnconfirmed
};

struct TimeSample {
  TimeSource src;
  int64_t utcMs;          // the source's UTC at sampleMonoMs
  uint64_t sampleMonoMs;  // monotonic ms when the sample was taken
  uint32_t accuracyMs;
  uint8_t flags;
};
constexpr uint8_t kTimeFlagOperator = 0x01;  // explicit user action (system.setTime)

struct TimeArbiterPolicy {
  uint32_t freshMs[kTimeSourceCount] = {0, 0, 0, 604800000UL, 7200000UL, 10800000UL};
  uint32_t stepMinMs[kTimeSourceCount] = {0, 0, 1000, 1500, 1000, 0};
  uint16_t holdDriftPpm = 30;
  uint32_t maxUncertaintyMs = 600000UL, staleAfterMs = 172800000UL;
  uint32_t crossCheckMs = 5000, disputeMs = 300000UL, bigJumpMs = 3600000UL;
  uint8_t bigJumpConfirm = 3;
  uint32_t bigJumpSpreadMs = 30000;
  int64_t minSaneUtcS = 1767225600LL;  // 2026-01-01
};

class TimeArbiter {
 public:
  struct Now {
    int64_t sysUtcMs;
    uint64_t monoMs;
  };
  struct Verdict {
    SubmitResult result;
    bool stepClock;   // caller must set the clock to setUtcMs
    int64_t setUtcMs;
    int32_t offsetMs;  // applied - sysUtc (clamped to int32)
    bool wasValid;     // clock validity BEFORE this sample (Stepped vs BecameValid)
  };

  // Accuracy credited to an SNTP sync reported through externalSync().
  static constexpr uint32_t kExternalNtpAccuracyMs = 250;
  // Upper plausibility bound: 2100-01-01T00:00:00Z.
  static constexpr int64_t kMaxSaneUtcS = 4102444800LL;

  explicit TimeArbiter(const TimeArbiterPolicy& p = {});

  Verdict submit(const TimeSample&, const Now&);

  // SNTP (or another external agent) already stepped the clock by
  // observedStepMs; `now` is AFTER the step. Implausible -> rejected and the
  // source is not refreshed.
  Verdict externalSync(TimeSource, const Now&, int32_t observedStepMs, bool plausible);

  // At boot: the system clock survived (RTC domain / persisted) — trust it as
  // RtcHold with acc + drift since lastSyncUtcMs. `last` is the source that
  // synced it (Ntp/Gps keep staleness honest across the reboot). No-op when
  // now.sysUtcMs is implausible.
  void restoreHold(TimeSource last, int64_t lastSyncUtcMs, uint32_t accMs, const Now&);

  bool valid(const Now&) const;
  uint32_t uncertaintyMs(const Now&) const;  // UINT32_MAX when nothing accepted
  bool stale(const Now&) const;
  bool disputed() const { return _disputed; }
  TimeSource authority(const Now&) const;
  int32_t offsetMs(TimeSource) const;
  const TimeArbiterPolicy& policy() const { return _p; }
  // Replaces the policy; accepted samples, holds and pending sets are kept.
  void setPolicy(const TimeArbiterPolicy& p) { _p = p; }

 private:
  struct Src {                // 48 B: 64-bit fields first, no padding holes
    uint64_t acceptedMono = 0;
    int64_t base = 0;          // implied utc at mono 0 of the last plausible sample
    uint64_t baseMono = 0;     // when that sample was taken
    int64_t pendBase = 0;      // big-jump confirmation
    uint64_t pendFirstMono = 0;
    int32_t offset = 0;
    uint8_t pendCount = 0;
    bool accepted = false;
    bool hasBase = false;
  };

  TimeArbiterPolicy _p;
  Src _s[kTimeSourceCount - 1];  // index = rank - 1 (None keeps no state)
  bool _hasRef = false;
  uint64_t _refMono = 0;
  uint32_t _refAcc = 0;
  bool _hasNtpGps = false;
  uint64_t _ntpGpsMono = 0;
  int64_t _ntpGpsAgeBiasMs = 0;  // restoreHold: age of the last Ntp/Gps sync at boot
  bool _disputed = false;

  bool plausible(int64_t utcMs) const;
  void accept(TimeSource src, const Now& now, uint32_t accMs);
  void recordBase(TimeSource src, int64_t base, uint64_t mono, int64_t delta);
  void evalDispute(const Now& now);
  bool freshBase(TimeSource src, const Now& now) const;
};

}  // namespace ESPRack

#endif  // ESPRACK_TIME_ARBITER_H
