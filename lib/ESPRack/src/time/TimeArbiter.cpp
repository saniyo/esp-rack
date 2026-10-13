#include "time/TimeArbiter.h"

namespace ESPRack {

namespace {
inline uint8_t rank(TimeSource s) { return static_cast<uint8_t>(s); }
inline int64_t absi(int64_t v) { return v < 0 ? -v : v; }
inline int32_t clamp32(int64_t v) {
  if (v > 2147483647LL) return 2147483647;
  if (v < -2147483647LL - 1) return -2147483647 - 1;
  return static_cast<int32_t>(v);
}
inline uint64_t monoAge(uint64_t now, uint64_t then) { return now >= then ? now - then : 0; }
}  // namespace

TimeArbiter::TimeArbiter(const TimeArbiterPolicy& p) : _p(p) {}

bool TimeArbiter::plausible(int64_t utcMs) const {
  const int64_t s = utcMs / 1000;
  return s >= _p.minSaneUtcS && s < kMaxSaneUtcS;
}

uint32_t TimeArbiter::uncertaintyMs(const Now& now) const {
  if (!_hasRef) return 0xFFFFFFFFu;
  const uint64_t age = monoAge(now.monoMs, _refMono);
  const uint64_t drift = age * _p.holdDriftPpm / 1000000ULL;
  const uint64_t u = static_cast<uint64_t>(_refAcc) + drift;
  return u > 0xFFFFFFFEull ? 0xFFFFFFFEu : static_cast<uint32_t>(u);
}

bool TimeArbiter::valid(const Now& now) const {
  return _hasRef && uncertaintyMs(now) < _p.maxUncertaintyMs;
}

bool TimeArbiter::stale(const Now& now) const {
  if (!_hasNtpGps) return true;
  const uint64_t age = monoAge(now.monoMs, _ntpGpsMono) + static_cast<uint64_t>(_ntpGpsAgeBiasMs);
  return age > _p.staleAfterMs;
}

TimeSource TimeArbiter::authority(const Now& now) const {
  for (uint8_t r = kTimeSourceCount - 1; r > 0; --r) {
    const Src& s = _s[r - 1];
    if (_p.freshMs[r] == 0 || !s.accepted) continue;
    if (monoAge(now.monoMs, s.acceptedMono) < _p.freshMs[r]) return static_cast<TimeSource>(r);
  }
  return TimeSource::None;
}

int32_t TimeArbiter::offsetMs(TimeSource src) const {
  const uint8_t r = rank(src);
  return (r > 0 && r < kTimeSourceCount) ? _s[r - 1].offset : 0;
}

void TimeArbiter::accept(TimeSource src, const Now& now, uint32_t accMs) {
  Src& s = _s[rank(src) - 1];
  s.accepted = true;
  s.acceptedMono = now.monoMs;
  _hasRef = true;
  _refMono = now.monoMs;
  _refAcc = accMs;
  if (src == TimeSource::Ntp || src == TimeSource::Gps) {
    _hasNtpGps = true;
    _ntpGpsMono = now.monoMs;
    _ntpGpsAgeBiasMs = 0;
  }
}

void TimeArbiter::recordBase(TimeSource src, int64_t base, uint64_t mono, int64_t delta) {
  Src& s = _s[rank(src) - 1];
  s.hasBase = true;
  s.base = base;
  s.baseMono = mono;
  s.offset = clamp32(delta);
}

bool TimeArbiter::freshBase(TimeSource src, const Now& now) const {
  const Src& s = _s[rank(src) - 1];
  const uint32_t fresh = _p.freshMs[rank(src)];
  return s.hasBase && fresh > 0 && monoAge(now.monoMs, s.baseMono) < fresh;
}

void TimeArbiter::evalDispute(const Now& now) {
  const Src& ntp = _s[rank(TimeSource::Ntp) - 1];
  const Src& gps = _s[rank(TimeSource::Gps) - 1];
  const Src& rtc = _s[rank(TimeSource::ExtRtc) - 1];
  _disputed = freshBase(TimeSource::Ntp, now) && freshBase(TimeSource::Gps, now) &&
              freshBase(TimeSource::ExtRtc, now) &&
              absi(ntp.base - gps.base) > static_cast<int64_t>(_p.disputeMs) &&
              absi(gps.base - rtc.base) <= static_cast<int64_t>(_p.crossCheckMs);
}

TimeArbiter::Verdict TimeArbiter::submit(const TimeSample& smp, const Now& now) {
  Verdict v{SubmitResult::RejectedImplausible, false, 0, 0, valid(now)};
  const uint8_t r = rank(smp.src);
  if (r == 0 || r >= kTimeSourceCount || !plausible(smp.utcMs)) return v;

  const int64_t applied =
      smp.utcMs + (static_cast<int64_t>(now.monoMs) - static_cast<int64_t>(smp.sampleMonoMs));
  const int64_t delta = applied - now.sysUtcMs;
  const int64_t base = smp.utcMs - static_cast<int64_t>(smp.sampleMonoMs);
  v.offsetMs = clamp32(delta);
  recordBase(smp.src, base, smp.sampleMonoMs, delta);
  evalDispute(now);

  const TimeSource auth = authority(now);
  const bool opFlag = (smp.flags & kTimeFlagOperator) != 0;
  const bool operatorOverride =
      opFlag && smp.src == TimeSource::Manual && auth != TimeSource::Gps && auth != TimeSource::Ntp;
  const bool eligible = !v.wasValid || r >= rank(auth) || stale(now) || operatorOverride;
  if (!eligible) {
    v.result = SubmitResult::RejectedOutranked;
    return v;
  }

  Src& s = _s[r - 1];
  const bool bigJump = v.wasValid && absi(delta) > static_cast<int64_t>(_p.bigJumpMs) &&
                       smp.src != TimeSource::Ntp && !opFlag;
  if (bigJump) {
    bool restarted = false;
    if (s.pendCount == 0 || absi(base - s.pendBase) > static_cast<int64_t>(_p.crossCheckMs)) {
      restarted = s.pendCount > 0;  // disagrees with the pending set: start over from it
      s.pendCount = 1;
      s.pendBase = base;
      s.pendFirstMono = smp.sampleMonoMs;
    } else if (s.pendCount < 255) {
      ++s.pendCount;
    }
    const uint64_t spread = monoAge(smp.sampleMonoMs, s.pendFirstMono);
    if (s.pendCount < _p.bigJumpConfirm || spread < _p.bigJumpSpreadMs) {
      v.result = restarted ? SubmitResult::RejectedUnconfirmed : SubmitResult::Pending;
      return v;
    }
    s.pendCount = 0;  // confirmed: fall through and apply the latest sample
  } else {
    s.pendCount = 0;
  }

  if (absi(delta) < static_cast<int64_t>(_p.stepMinMs[r])) {
    v.result = SubmitResult::Confirmed;
  } else {
    v.result = SubmitResult::Applied;
    v.stepClock = true;
    v.setUtcMs = applied;
  }
  accept(smp.src, now, smp.accuracyMs);
  return v;
}

TimeArbiter::Verdict TimeArbiter::externalSync(TimeSource src, const Now& now,
                                               int32_t observedStepMs, bool isPlausible) {
  // validity BEFORE the step: the reference is unchanged by the external step
  Verdict v{SubmitResult::RejectedImplausible, false, 0, observedStepMs, valid(now)};
  const uint8_t r = rank(src);
  if (r == 0 || r >= kTimeSourceCount || !isPlausible || !plausible(now.sysUtcMs)) return v;
  recordBase(src, now.sysUtcMs - static_cast<int64_t>(now.monoMs), now.monoMs, observedStepMs);
  evalDispute(now);
  _s[r - 1].pendCount = 0;
  v.result = (observedStepMs > -static_cast<int32_t>(_p.stepMinMs[r]) &&
              observedStepMs < static_cast<int32_t>(_p.stepMinMs[r]))
                 ? SubmitResult::Confirmed
                 : SubmitResult::Applied;
  v.setUtcMs = now.sysUtcMs;  // already applied by the external agent
  accept(src, now, kExternalNtpAccuracyMs);
  return v;
}

void TimeArbiter::restoreHold(TimeSource last, int64_t lastSyncUtcMs, uint32_t accMs, const Now& now) {
  if (!plausible(now.sysUtcMs)) return;  // the clock did not survive: nothing to hold
  const int64_t age = now.sysUtcMs > lastSyncUtcMs ? now.sysUtcMs - lastSyncUtcMs : 0;
  const uint64_t drift = static_cast<uint64_t>(age) * _p.holdDriftPpm / 1000000ULL;
  const uint64_t acc = static_cast<uint64_t>(accMs) + drift;
  Src& s = _s[rank(TimeSource::RtcHold) - 1];
  s.accepted = true;
  s.acceptedMono = now.monoMs;
  _hasRef = true;
  _refMono = now.monoMs;
  _refAcc = acc > 0xFFFFFFFEull ? 0xFFFFFFFEu : static_cast<uint32_t>(acc);
  if (last == TimeSource::Ntp || last == TimeSource::Gps) {
    _hasNtpGps = true;
    _ntpGpsMono = now.monoMs;
    _ntpGpsAgeBiasMs = age;
  }
}

}  // namespace ESPRack
