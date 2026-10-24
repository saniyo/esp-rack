// TimeArbiter matrix — rank × freshness × plausibility
// × step threshold × big jump × dispute × operator flag; externalSync, restoreHold.
#include <unity.h>

#include <time/TimeArbiter.h>

using namespace ESPRack;
using Now = TimeArbiter::Now;
using Verdict = TimeArbiter::Verdict;

void setUp(void) {}
void tearDown(void) {}

static constexpr int64_t T0 = 1791936000000LL;  // 2026-10-14T00:00:00Z
static constexpr int64_t J = 7200000;           // a 2 h jump (> bigJumpMs)
static constexpr uint8_t OP = kTimeFlagOperator;

#define ASSERT_RES(exp, v) \
  TEST_ASSERT_EQUAL_INT_MESSAGE((int)SubmitResult::exp, (int)(v).result, "expected " #exp)
#define ASSERT_SRC(exp, s) TEST_ASSERT_EQUAL_INT((int)TimeSource::exp, (int)(s))

static TimeSample smp(TimeSource src, int64_t utcMs, uint64_t mono, uint32_t acc = 100, uint8_t flags = 0) {
  return TimeSample{src, utcMs, mono, acc, flags};
}
// The system clock reads true time (T0 + mono); the sample is `d` ms ahead of it.
static Now now(uint64_t mono) { return Now{T0 + (int64_t)mono, mono}; }
static Verdict at(TimeArbiter& a, TimeSource src, uint64_t mono, int64_t d, uint8_t flags = 0) {
  return a.submit(smp(src, T0 + (int64_t)mono + d, mono, 100, flags), now(mono));
}
// First sync on an unset clock (1970): always Applied.
static void prime(TimeArbiter& a, TimeSource src, uint64_t mono) {
  const Verdict v = a.submit(smp(src, T0 + (int64_t)mono, mono), Now{0, mono});
  ASSERT_RES(Applied, v);
  TEST_ASSERT_FALSE(v.wasValid);
}

static void test_initial_state(void) {
  TimeArbiter a;
  TEST_ASSERT_FALSE(a.valid(now(1000)));
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, a.uncertaintyMs(now(1000)));
  TEST_ASSERT_TRUE(a.stale(now(1000)));
  TEST_ASSERT_FALSE(a.disputed());
  ASSERT_SRC(None, a.authority(now(1000)));
  TEST_ASSERT_EQUAL_INT64(0, a.offsetMs(TimeSource::Gps));
}

static void test_implausible(void) {
  TimeArbiter a;
  const int64_t minSane = a.policy().minSaneUtcS * 1000;
  Verdict v = a.submit(smp(TimeSource::Gps, minSane - 1000, 0), Now{0, 0});
  ASSERT_RES(RejectedImplausible, v);
  TEST_ASSERT_FALSE(v.stepClock);
  v = a.submit(smp(TimeSource::Ntp, TimeArbiter::kMaxSaneUtcS * 1000, 0), Now{0, 0});
  ASSERT_RES(RejectedImplausible, v);
  v = a.submit(smp(TimeSource::None, T0, 0), Now{0, 0});
  ASSERT_RES(RejectedImplausible, v);
  TEST_ASSERT_FALSE(a.valid(Now{0, 0}));
  TEST_ASSERT_EQUAL_INT64(0, a.offsetMs(TimeSource::Gps));  // implausible: offset not recorded
  ASSERT_SRC(None, a.authority(Now{0, 0}));

  TimeArbiter b;  // both bounds inclusive/exclusive as documented
  ASSERT_RES(Applied, b.submit(smp(TimeSource::Gps, minSane, 0), Now{0, 0}));
  TimeArbiter c;
  ASSERT_RES(Applied, c.submit(smp(TimeSource::Gps, TimeArbiter::kMaxSaneUtcS * 1000 - 1, 0), Now{0, 0}));
}

static void test_first_sample_mono_compensated(void) {
  TimeArbiter a;
  // taken at mono 1000, submitted at mono 1250 on an unset clock
  const Verdict v = a.submit(smp(TimeSource::Gps, T0 + 1000, 1000, 80), Now{0, 1250});
  ASSERT_RES(Applied, v);
  TEST_ASSERT_TRUE(v.stepClock);
  TEST_ASSERT_EQUAL_INT64(T0 + 1250, v.setUtcMs);
  TEST_ASSERT_FALSE(v.wasValid);
  TEST_ASSERT_EQUAL_INT64(T0 + 1250, v.offsetMs);  // from 1970: exact, no clamp
  TEST_ASSERT_TRUE(a.valid(now(1250)));
  TEST_ASSERT_EQUAL_UINT32(80, a.uncertaintyMs(now(1250)));
  ASSERT_SRC(Gps, a.authority(now(1250)));
  TEST_ASSERT_FALSE(a.stale(now(1250)));
  TEST_ASSERT_EQUAL_INT64(T0 + 1250, a.offsetMs(TimeSource::Gps));
}

static void test_step_thresholds(void) {
  TimeArbiter g;  // Gps: 1000 ms
  prime(g, TimeSource::Gps, 0);
  Verdict v = at(g, TimeSource::Gps, 1000, 999);
  ASSERT_RES(Confirmed, v);
  TEST_ASSERT_FALSE(v.stepClock);
  TEST_ASSERT_TRUE(v.wasValid);
  ASSERT_RES(Confirmed, at(g, TimeSource::Gps, 2000, -999));
  v = at(g, TimeSource::Gps, 3000, 1000);
  ASSERT_RES(Applied, v);
  TEST_ASSERT_TRUE(v.stepClock);
  TEST_ASSERT_EQUAL_INT64(T0 + 4000, v.setUtcMs);
  TEST_ASSERT_EQUAL_INT64(1000, v.offsetMs);
  ASSERT_RES(Applied, at(g, TimeSource::Gps, 4000, -1000));

  TimeArbiter r;  // ExtRtc: 1500 ms
  prime(r, TimeSource::ExtRtc, 0);
  ASSERT_RES(Confirmed, at(r, TimeSource::ExtRtc, 1000, 1499));
  ASSERT_RES(Confirmed, at(r, TimeSource::ExtRtc, 2000, -1499));
  v = at(r, TimeSource::ExtRtc, 3000, 1500);
  ASSERT_RES(Applied, v);
  TEST_ASSERT_EQUAL_INT64(T0 + 4500, v.setUtcMs);

  TimeArbiter m;  // Manual: 1000 ms
  prime(m, TimeSource::Manual, 0);
  ASSERT_RES(Confirmed, at(m, TimeSource::Manual, 1000, 999));
  ASSERT_RES(Applied, at(m, TimeSource::Manual, 2000, 1000));

  TimeArbiter n;  // Ntp: 0 ms — always steps
  prime(n, TimeSource::Ntp, 0);
  v = at(n, TimeSource::Ntp, 1000, 0);
  ASSERT_RES(Applied, v);
  TEST_ASSERT_TRUE(v.stepClock);
  TEST_ASSERT_EQUAL_INT64(T0 + 1000, v.setUtcMs);
  ASSERT_RES(Applied, at(n, TimeSource::Ntp, 2000, 1));
}

static void test_rank_outranked_offset_recorded(void) {
  TimeArbiter a;
  prime(a, TimeSource::Ntp, 0);
  Verdict v = at(a, TimeSource::Gps, 1000, 3000);
  ASSERT_RES(RejectedOutranked, v);
  TEST_ASSERT_FALSE(v.stepClock);
  TEST_ASSERT_TRUE(v.wasValid);
  TEST_ASSERT_EQUAL_INT64(3000, v.offsetMs);
  TEST_ASSERT_EQUAL_INT64(3000, a.offsetMs(TimeSource::Gps));
  ASSERT_RES(RejectedOutranked, at(a, TimeSource::ExtRtc, 1000, -2000));
  TEST_ASSERT_EQUAL_INT64(-2000, a.offsetMs(TimeSource::ExtRtc));
  ASSERT_RES(RejectedOutranked, at(a, TimeSource::Manual, 1000, 5000));
  ASSERT_RES(RejectedOutranked, at(a, TimeSource::Manual, 1000, 5000, OP));  // NTP authority
  ASSERT_RES(Applied, at(a, TimeSource::Ntp, 2000, 10));
  ASSERT_SRC(Ntp, a.authority(now(2000)));

  const uint64_t ntpEnd = 2000 + a.policy().freshMs[5];
  ASSERT_SRC(Ntp, a.authority(now(ntpEnd - 1)));
  ASSERT_RES(RejectedOutranked, at(a, TimeSource::Gps, ntpEnd - 1, 3000));
  ASSERT_SRC(None, a.authority(now(ntpEnd)));
  ASSERT_RES(Applied, at(a, TimeSource::Gps, ntpEnd, 3000));
  ASSERT_SRC(Gps, a.authority(now(ntpEnd)));
}

static void test_freshness_falls_through_ranks(void) {
  TimeArbiter a;
  prime(a, TimeSource::ExtRtc, 0);
  ASSERT_RES(Confirmed, at(a, TimeSource::Gps, 1000, 0));
  ASSERT_SRC(Gps, a.authority(now(1000 + 7200000 - 1)));
  ASSERT_SRC(ExtRtc, a.authority(now(1000 + 7200000)));
  ASSERT_SRC(ExtRtc, a.authority(now(604800000 - 1)));
  ASSERT_SRC(None, a.authority(now(604800000)));

  TimeArbiter n;
  prime(n, TimeSource::Ntp, 0);
  ASSERT_SRC(Ntp, n.authority(now(10800000 - 1)));
  ASSERT_SRC(None, n.authority(now(10800000)));

  TimeArbiter m;  // Manual / RtcHold are never authority
  prime(m, TimeSource::Manual, 0);
  ASSERT_SRC(None, m.authority(now(0)));
}

static void test_operator_manual(void) {
  TimeArbiter a;
  prime(a, TimeSource::Gps, 0);
  ASSERT_RES(Confirmed, at(a, TimeSource::ExtRtc, 7200000, 0));  // GPS no longer fresh
  ASSERT_SRC(ExtRtc, a.authority(now(7200000)));
  TEST_ASSERT_FALSE(a.stale(now(7300000)));
  ASSERT_RES(RejectedOutranked, at(a, TimeSource::Manual, 7300000, 600000));
  Verdict v = at(a, TimeSource::Manual, 7300000, 600000, OP);  // operator outranks ExtRtc
  ASSERT_RES(Applied, v);
  TEST_ASSERT_TRUE(v.stepClock);
  TEST_ASSERT_EQUAL_INT64(T0 + 7300000 + 600000, v.setUtcMs);
  ASSERT_RES(Applied, at(a, TimeSource::Manual, 7400000, J, OP));  // and is exempt from big jump
  ASSERT_SRC(ExtRtc, a.authority(now(7400000)));

  TimeArbiter g;  // ... but never GPS or NTP
  prime(g, TimeSource::Gps, 0);
  ASSERT_RES(RejectedOutranked, at(g, TimeSource::Manual, 1000, 600000, OP));
  ASSERT_RES(RejectedOutranked, at(g, TimeSource::ExtRtc, 1000, 5000, OP));  // flag is Manual-only
  TimeArbiter n;
  prime(n, TimeSource::Ntp, 0);
  ASSERT_RES(RejectedOutranked, at(n, TimeSource::Manual, 1000, 600000, OP));
}

static void test_stale_makes_eligible(void) {
  TimeArbiter a;
  prime(a, TimeSource::Gps, 0);
  ASSERT_RES(Confirmed, at(a, TimeSource::ExtRtc, 7200000, 0));
  TEST_ASSERT_FALSE(a.stale(now(172800000)));
  TEST_ASSERT_TRUE(a.stale(now(172800001)));
  ASSERT_RES(RejectedOutranked, at(a, TimeSource::Manual, 172800000, 5000));
  ASSERT_RES(Applied, at(a, TimeSource::Manual, 172800001, 5000));
  ASSERT_SRC(ExtRtc, a.authority(now(172800001)));
  TEST_ASSERT_TRUE(a.stale(now(172800001)));  // Manual does not refresh staleness
}

static void test_validity_uncertainty(void) {
  TimeArbiter a;
  prime(a, TimeSource::Gps, 0);  // acc 100, 30 ppm
  TEST_ASSERT_EQUAL_UINT32(100, a.uncertaintyMs(now(0)));
  TEST_ASSERT_EQUAL_UINT32(130, a.uncertaintyMs(now(1000000)));
  TEST_ASSERT_TRUE(a.valid(now(19996666666ULL)));
  TEST_ASSERT_EQUAL_UINT32(599999, a.uncertaintyMs(now(19996666666ULL)));
  TEST_ASSERT_FALSE(a.valid(now(19996666667ULL)));
  TEST_ASSERT_EQUAL_UINT32(600000, a.uncertaintyMs(now(19996666667ULL)));
  // an invalid clock takes any plausible sample, a big jump included
  const Verdict v = at(a, TimeSource::Gps, 19996666667ULL, J);
  ASSERT_RES(Applied, v);
  TEST_ASSERT_FALSE(v.wasValid);
  TEST_ASSERT_TRUE(v.stepClock);
  TEST_ASSERT_EQUAL_INT64(T0 + 19996666667LL + J, v.setUtcMs);
  TEST_ASSERT_TRUE(a.valid(now(19996666667ULL)));
}

static void test_big_jump_confirm(void) {
  TimeArbiter a;
  prime(a, TimeSource::Gps, 0);
  Verdict v = at(a, TimeSource::Gps, 10000, J);
  ASSERT_RES(Pending, v);
  TEST_ASSERT_FALSE(v.stepClock);
  TEST_ASSERT_TRUE(v.wasValid);
  TEST_ASSERT_EQUAL_INT64(J, v.offsetMs);
  TEST_ASSERT_EQUAL_INT64(J, a.offsetMs(TimeSource::Gps));
  ASSERT_RES(Pending, at(a, TimeSource::Gps, 20000, J));
  ASSERT_RES(Pending, at(a, TimeSource::Gps, 30000, J));  // 3 samples, spread 20 s
  v = at(a, TimeSource::Gps, 40000, J);                    // spread 30 s
  ASSERT_RES(Applied, v);
  TEST_ASSERT_TRUE(v.stepClock);
  TEST_ASSERT_TRUE(v.wasValid);
  TEST_ASSERT_EQUAL_INT64(T0 + 40000 + J, v.setUtcMs);
  // the stepped clock now agrees
  ASSERT_RES(Confirmed, a.submit(smp(TimeSource::Gps, T0 + 50000 + J, 50000), Now{T0 + 50000 + J, 50000}));

  TimeArbiter b;
  prime(b, TimeSource::Gps, 0);
  ASSERT_RES(Pending, at(b, TimeSource::Gps, 10000, -J));
  ASSERT_SRC(Gps, b.authority(now(10000)));
  TEST_ASSERT_TRUE(b.valid(now(10000)));
}

static void test_big_jump_tolerance_and_disagreement(void) {
  TimeArbiter a;
  prime(a, TimeSource::Gps, 0);
  ASSERT_RES(Pending, at(a, TimeSource::Gps, 10000, J));
  ASSERT_RES(Pending, at(a, TimeSource::Gps, 20000, J + 5000));  // within crossCheckMs
  const Verdict v = at(a, TimeSource::Gps, 40000, J - 3000);
  ASSERT_RES(Applied, v);
  TEST_ASSERT_EQUAL_INT64(T0 + 40000 + J - 3000, v.setUtcMs);

  TimeArbiter b;
  prime(b, TimeSource::Gps, 0);
  ASSERT_RES(Pending, at(b, TimeSource::Gps, 10000, J));
  Verdict w = at(b, TimeSource::Gps, 20000, J + 5001);  // disagrees: restart from it
  ASSERT_RES(RejectedUnconfirmed, w);
  TEST_ASSERT_FALSE(w.stepClock);
  ASSERT_RES(Pending, at(b, TimeSource::Gps, 30000, J + 5001));
  ASSERT_RES(Pending, at(b, TimeSource::Gps, 40000, J + 5001));  // 3 samples, spread 20 s
  w = at(b, TimeSource::Gps, 50000, J + 5001);
  ASSERT_RES(Applied, w);
  TEST_ASSERT_EQUAL_INT64(T0 + 50000 + J + 5001, w.setUtcMs);
}

static void test_big_jump_cleared_by_normal_sample(void) {
  TimeArbiter a;
  prime(a, TimeSource::Gps, 0);
  ASSERT_RES(Pending, at(a, TimeSource::Gps, 10000, J));
  ASSERT_RES(Pending, at(a, TimeSource::Gps, 20000, J));
  ASSERT_RES(Confirmed, at(a, TimeSource::Gps, 25000, 0));  // clears the pending set
  ASSERT_RES(Pending, at(a, TimeSource::Gps, 30000, J));
  ASSERT_RES(Pending, at(a, TimeSource::Gps, 40000, J));
  ASSERT_RES(Pending, at(a, TimeSource::Gps, 50000, J));
  ASSERT_RES(Applied, at(a, TimeSource::Gps, 60000, J));
}

static void test_big_jump_exemptions(void) {
  TimeArbiter a;
  prime(a, TimeSource::Gps, 0);
  ASSERT_RES(Applied, at(a, TimeSource::Gps, 1000, 3600000));  // == bigJumpMs is not a jump
  ASSERT_RES(Pending, at(a, TimeSource::Gps, 2000, 3600001));

  TimeArbiter n;
  prime(n, TimeSource::Gps, 0);
  const Verdict v = at(n, TimeSource::Ntp, 1000, J);  // NTP is never held back
  ASSERT_RES(Applied, v);
  TEST_ASSERT_EQUAL_INT64(T0 + 1000 + J, v.setUtcMs);

  TimeArbiter m;
  prime(m, TimeSource::ExtRtc, 0);
  ASSERT_RES(Pending, at(m, TimeSource::Manual, 1000, J));
  ASSERT_RES(Applied, at(m, TimeSource::Manual, 2000, J, OP));
  ASSERT_RES(Pending, at(m, TimeSource::ExtRtc, 3000, J));
}

static void test_big_jump_custom_policy(void) {
  TimeArbiterPolicy p;
  p.bigJumpConfirm = 1;
  p.bigJumpSpreadMs = 0;
  TimeArbiter a(p);
  TEST_ASSERT_EQUAL_UINT8(1, a.policy().bigJumpConfirm);
  prime(a, TimeSource::Gps, 0);
  ASSERT_RES(Applied, at(a, TimeSource::Gps, 1000, J));

  p.bigJumpConfirm = 2;
  TimeArbiter b(p);
  prime(b, TimeSource::Gps, 0);
  ASSERT_RES(Pending, at(b, TimeSource::Gps, 1000, J));
  ASSERT_RES(Applied, at(b, TimeSource::Gps, 1001, J));

  // setPolicy keeps the state: still valid, the pending set survives
  TimeArbiter c;
  prime(c, TimeSource::Gps, 0);
  ASSERT_RES(Pending, at(c, TimeSource::Gps, 1000, J));
  TimeArbiterPolicy q;
  q.bigJumpConfirm = 2;
  q.bigJumpSpreadMs = 0;
  c.setPolicy(q);
  TEST_ASSERT_EQUAL_UINT8(2, c.policy().bigJumpConfirm);
  TEST_ASSERT_TRUE(c.valid(now(2000)));
  ASSERT_RES(Applied, at(c, TimeSource::Gps, 2000, J));
}

static void test_dispute(void) {
  TimeArbiter a;
  prime(a, TimeSource::ExtRtc, 1000);                      // RTC base T0
  ASSERT_RES(Applied, at(a, TimeSource::Gps, 2000, 3000));  // GPS base T0+3 s
  TEST_ASSERT_FALSE(a.disputed());
  // SNTP stepped the clock to +400 s
  const Verdict v = a.externalSync(TimeSource::Ntp, Now{T0 + 3000 + 400000, 3000}, 400000, true);
  ASSERT_RES(Applied, v);
  TEST_ASSERT_FALSE(v.stepClock);
  TEST_ASSERT_EQUAL_INT64(400000, v.offsetMs);
  TEST_ASSERT_TRUE(v.wasValid);
  TEST_ASSERT_TRUE(a.disputed());
  ASSERT_SRC(Ntp, a.authority(now(3000)));

  at(a, TimeSource::Ntp, 4000, 3000 + 300000);  // exactly disputeMs: not disputed
  TEST_ASSERT_FALSE(a.disputed());
  at(a, TimeSource::Ntp, 5000, 3000 + 300001);
  TEST_ASSERT_TRUE(a.disputed());
  // outranked GPS samples still feed the 2-of-3 check
  ASSERT_RES(RejectedOutranked, at(a, TimeSource::Gps, 6000, 3000 + 300001));
  TEST_ASSERT_FALSE(a.disputed());  // GPS now disagrees with the RTC
  ASSERT_RES(RejectedOutranked, at(a, TimeSource::Gps, 7000, 3000));
  TEST_ASSERT_TRUE(a.disputed());
  // GPS no longer fresh -> no 2-of-3
  at(a, TimeSource::Ntp, 7000 + 7200000, 3000 + 300001);
  TEST_ASSERT_FALSE(a.disputed());
}

static void test_dispute_requires_gps_rtc_agreement(void) {
  TimeArbiter a;
  prime(a, TimeSource::ExtRtc, 1000);
  ASSERT_RES(Applied, at(a, TimeSource::Gps, 2000, 6000));  // GPS vs RTC 6 s apart
  ASSERT_RES(Applied, at(a, TimeSource::Ntp, 3000, 400000));
  TEST_ASSERT_FALSE(a.disputed());

  TimeArbiter b;  // no RTC at all
  prime(b, TimeSource::Gps, 0);
  ASSERT_RES(Applied, at(b, TimeSource::Ntp, 1000, 400000));
  TEST_ASSERT_FALSE(b.disputed());
}

// steps beyond int32 ms (24.8 days) come through exactly.
static constexpr int64_t kDay = 86400000LL;

static void test_step_int64(void) {
  TimeArbiter a;
  prime(a, TimeSource::Ntp, 1000);
  // the clock was set 40 days back by hand; NTP brings it forward in one step
  Verdict v = a.submit(smp(TimeSource::Ntp, T0 + 2000, 2000), Now{T0 + 2000 - 40 * kDay, 2000});
  ASSERT_RES(Applied, v);
  TEST_ASSERT_TRUE(v.stepClock);
  TEST_ASSERT_TRUE(v.wasValid);
  TEST_ASSERT_EQUAL_INT64(40 * kDay, v.offsetMs);
  TEST_ASSERT_EQUAL_INT64(T0 + 2000, v.setUtcMs);
  TEST_ASSERT_EQUAL_INT64(40 * kDay, a.offsetMs(TimeSource::Ntp));
  // ... and 400 days ahead: back in one step
  v = a.submit(smp(TimeSource::Ntp, T0 + 3000, 3000), Now{T0 + 3000 + 400 * kDay, 3000});
  ASSERT_RES(Applied, v);
  TEST_ASSERT_EQUAL_INT64(-400 * kDay, v.offsetMs);
  TEST_ASSERT_EQUAL_INT64(-400 * kDay, a.offsetMs(TimeSource::Ntp));
}

static void test_external_step_int64(void) {
  TimeArbiter a;
  Verdict v = a.externalSync(TimeSource::Ntp, Now{T0, 5000}, 40 * kDay, true);
  ASSERT_RES(Applied, v);
  TEST_ASSERT_EQUAL_INT64(40 * kDay, v.offsetMs);
  TEST_ASSERT_EQUAL_INT64(40 * kDay, a.offsetMs(TimeSource::Ntp));
  v = a.externalSync(TimeSource::Ntp, Now{T0 + 1000, 6000}, -400 * kDay, true);
  ASSERT_RES(Applied, v);
  TEST_ASSERT_TRUE(v.wasValid);
  TEST_ASSERT_EQUAL_INT64(-400 * kDay, v.offsetMs);
  TEST_ASSERT_EQUAL_INT64(-400 * kDay, a.offsetMs(TimeSource::Ntp));
  // a sub-threshold step is still just Confirmed (the threshold compare is 64-bit too)
  TimeArbiter g;
  v = g.externalSync(TimeSource::Gps, Now{T0, 5000}, 500, true);
  ASSERT_RES(Confirmed, v);
}

static void test_external_sync(void) {
  TimeArbiter a;
  Verdict v = a.externalSync(TimeSource::Ntp, Now{T0, 5000}, 1234, false);
  ASSERT_RES(RejectedImplausible, v);
  v = a.externalSync(TimeSource::Ntp, Now{1000, 5000}, 0, true);  // clock itself implausible
  ASSERT_RES(RejectedImplausible, v);
  TEST_ASSERT_FALSE(a.valid(Now{T0, 5000}));
  ASSERT_SRC(None, a.authority(Now{T0, 5000}));
  TEST_ASSERT_TRUE(a.stale(Now{T0, 5000}));  // implausible never refreshes NTP

  v = a.externalSync(TimeSource::Ntp, Now{T0, 5000}, 1234, true);
  ASSERT_RES(Applied, v);
  TEST_ASSERT_FALSE(v.stepClock);
  TEST_ASSERT_FALSE(v.wasValid);
  TEST_ASSERT_EQUAL_INT64(T0, v.setUtcMs);
  TEST_ASSERT_EQUAL_INT64(1234, v.offsetMs);
  TEST_ASSERT_TRUE(a.valid(Now{T0, 5000}));
  TEST_ASSERT_EQUAL_UINT32(TimeArbiter::kExternalNtpAccuracyMs, a.uncertaintyMs(Now{T0, 5000}));
  ASSERT_SRC(Ntp, a.authority(Now{T0, 5000}));
  TEST_ASSERT_FALSE(a.stale(Now{T0, 5000}));
  TEST_ASSERT_EQUAL_INT64(1234, a.offsetMs(TimeSource::Ntp));
  v = a.externalSync(TimeSource::Ntp, Now{T0 + 1000, 6000}, -20, true);
  ASSERT_RES(Applied, v);
  TEST_ASSERT_TRUE(v.wasValid);
}

static void test_restore_hold(void) {
  const Now n{T0, 100};
  TimeArbiter a;
  a.restoreHold(TimeSource::Ntp, T0 - 86400000, 50, n);  // synced a day ago
  TEST_ASSERT_TRUE(a.valid(n));
  TEST_ASSERT_EQUAL_UINT32(50 + 2592, a.uncertaintyMs(n));
  ASSERT_SRC(None, a.authority(n));  // RtcHold is never authority
  TEST_ASSERT_FALSE(a.stale(Now{T0, 100 + 86400000}));
  TEST_ASSERT_TRUE(a.stale(Now{T0, 100 + 86400001}));
  // a held clock is valid: a GPS big jump must be confirmed, a small step applies
  ASSERT_RES(Pending, a.submit(smp(TimeSource::Gps, T0 + 900 + J, 1000), Now{T0 + 900, 1000}));
  ASSERT_RES(Applied, a.submit(smp(TimeSource::Gps, T0 + 1900 + 2000, 2000), Now{T0 + 1900, 2000}));

  TimeArbiter b;  // last sync by Manual: staleness is not carried
  b.restoreHold(TimeSource::Manual, T0 - 1000, 50, n);
  TEST_ASSERT_TRUE(b.valid(n));
  TEST_ASSERT_TRUE(b.stale(n));

  TimeArbiter c;  // the clock did not survive
  c.restoreHold(TimeSource::Ntp, T0 - 1000, 50, Now{5000, 100});
  TEST_ASSERT_FALSE(c.valid(Now{5000, 100}));

  TimeArbiter d;  // too long ago: drift exceeds maxUncertaintyMs
  d.restoreHold(TimeSource::Gps, T0 - 25000000000LL, 50, n);
  TEST_ASSERT_EQUAL_UINT32(50 + 750000, d.uncertaintyMs(n));
  TEST_ASSERT_FALSE(d.valid(n));
  TEST_ASSERT_TRUE(d.stale(n));

  TimeArbiter e;  // last sync "in the future": no drift credited
  e.restoreHold(TimeSource::Ntp, T0 + 5000, 70, n);
  TEST_ASSERT_EQUAL_UINT32(70, e.uncertaintyMs(n));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_initial_state);
  RUN_TEST(test_implausible);
  RUN_TEST(test_first_sample_mono_compensated);
  RUN_TEST(test_step_thresholds);
  RUN_TEST(test_rank_outranked_offset_recorded);
  RUN_TEST(test_freshness_falls_through_ranks);
  RUN_TEST(test_operator_manual);
  RUN_TEST(test_stale_makes_eligible);
  RUN_TEST(test_validity_uncertainty);
  RUN_TEST(test_big_jump_confirm);
  RUN_TEST(test_big_jump_tolerance_and_disagreement);
  RUN_TEST(test_big_jump_cleared_by_normal_sample);
  RUN_TEST(test_big_jump_exemptions);
  RUN_TEST(test_big_jump_custom_policy);
  RUN_TEST(test_dispute);
  RUN_TEST(test_dispute_requires_gps_rtc_agreement);
  RUN_TEST(test_external_sync);
  RUN_TEST(test_step_int64);
  RUN_TEST(test_external_step_int64);
  RUN_TEST(test_restore_hold);
  return UNITY_END();
}
