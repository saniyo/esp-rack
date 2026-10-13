// SES-LIB-006 acceptance 4: PosixTz vs IANA tzdata (fixture from
// scripts/gen_tz_fixture.py) on 10 000 instants and every 2024–2040
// transition; gap/fold 2026-03-29 and 2026-10-25; parser accept/reject.
#include <stdio.h>
#include <unity.h>

#include <ESPRackPure/Civil.h>
#include <ESPRackPure/PosixTz.h>
#include <tz_fixture.h>

using namespace ESPRack;
using Gap = PosixTz::Gap;
using Fold = PosixTz::Fold;

void setUp(void) {}
void tearDown(void) {}

static uint64_t splitmix64(uint64_t& state) {
  state += 0x9E3779B97F4A7C15ULL;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

static CivilTime civilOf(int64_t localS) {
  const int64_t days = civil::floorDiv(localS, 86400);
  const int64_t sod = localS - days * 86400;
  int32_t y;
  uint32_t m, d;
  civil::civilFromDays(days, y, m, d);
  return CivilTime{y, (uint8_t)m, (uint8_t)d, (uint8_t)(sod / 3600), (uint8_t)(sod / 60 % 60),
                   (uint8_t)(sod % 60), civil::weekdayMon0(days)};
}

static bool sameCivil(const CivilTime& a, const CivilTime& b) {
  return a.y == b.y && a.mo == b.mo && a.d == b.d && a.h == b.h && a.mi == b.mi && a.s == b.s &&
         a.wdayMon0 == b.wdayMon0;
}

static PosixTz zone(int i) {
  PosixTz tz;
  TEST_ASSERT_TRUE_MESSAGE(tz.parse(tzfx::kZones[i].posix), tzfx::kZones[i].name);
  return tz;
}

static int64_t U(int32_t y, uint32_t mo, uint32_t d, uint32_t h, uint32_t mi, uint32_t s) {
  return civil::utcFromCivil(y, mo, d, h, mi, s);
}
static CivilTime L(int32_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s) {
  return CivilTime{y, mo, d, h, mi, s, 0};
}

static void test_parse_valid(void) {
  PosixTz tz;
  TEST_ASSERT_TRUE(tz.parse("UTC0"));
  TEST_ASSERT_EQUAL_INT32(0, tz.stdOffset());
  TEST_ASSERT_FALSE(tz.hasDst());
  TEST_ASSERT_TRUE(tz.parse("JST-9"));
  TEST_ASSERT_EQUAL_INT32(32400, tz.offsetAt(U(2026, 7, 1, 0, 0, 0)));
  TEST_ASSERT_TRUE(tz.parse("<+0330>-3:30"));
  TEST_ASSERT_EQUAL_INT32(12600, tz.stdOffset());
  TEST_ASSERT_TRUE(tz.parse("<-03>3"));
  TEST_ASSERT_EQUAL_INT32(-10800, tz.stdOffset());
  TEST_ASSERT_TRUE(tz.parse("<-0230>+2:30:15"));
  TEST_ASSERT_EQUAL_INT32(-9015, tz.stdOffset());

  // DST without rules: POSIX default M3.2.0,M11.1.0 at 02:00, dst = std + 1 h
  TEST_ASSERT_TRUE(tz.parse("EST5EDT"));
  TEST_ASSERT_TRUE(tz.hasDst());
  TEST_ASSERT_EQUAL_INT32(-14400, tz.dstOffset());
  TEST_ASSERT_FALSE(tz.isDst(U(2026, 3, 8, 7, 0, 0) - 1));  // 2026-03-08 02:00 EST
  TEST_ASSERT_TRUE(tz.isDst(U(2026, 3, 8, 7, 0, 0)));
  TEST_ASSERT_TRUE(tz.isDst(U(2026, 11, 1, 6, 0, 0) - 1));  // 2026-11-01 02:00 EDT
  TEST_ASSERT_FALSE(tz.isDst(U(2026, 11, 1, 6, 0, 0)));

  TEST_ASSERT_TRUE(tz.parse("EST5EDT4,M3.2.0/2:00:00,M11.1.0/2:00:00"));
  TEST_ASSERT_TRUE(tz.isDst(U(2026, 3, 8, 7, 0, 0)));

  // Jn never counts Feb 29; n does
  TEST_ASSERT_TRUE(tz.parse("AAA0BBB,J60/0,J300/0"));
  TEST_ASSERT_FALSE(tz.isDst(U(2028, 2, 29, 23, 59, 59)));
  TEST_ASSERT_TRUE(tz.isDst(U(2028, 3, 1, 0, 0, 0)));
  TEST_ASSERT_TRUE(tz.isDst(U(2027, 3, 1, 0, 0, 0)));
  TEST_ASSERT_FALSE(tz.isDst(U(2027, 2, 28, 23, 59, 59)));
  TEST_ASSERT_TRUE(tz.parse("AAA0BBB,59/0,299/0"));
  TEST_ASSERT_TRUE(tz.isDst(U(2028, 2, 29, 0, 0, 0)));
  TEST_ASSERT_FALSE(tz.isDst(U(2028, 2, 28, 23, 59, 59)));
  TEST_ASSERT_FALSE(tz.isDst(U(2027, 2, 28, 23, 59, 59)));
  TEST_ASSERT_TRUE(tz.isDst(U(2027, 3, 1, 0, 0, 0)));

  // RFC 8536 transition-time range and the "all year DST" idiom
  TEST_ASSERT_TRUE(tz.parse("AAA0BBB,M3.5.0/-167,M10.5.0/167"));
  TEST_ASSERT_TRUE(tz.parse("EST5EDT,0/0,J365/25"));
  for (int64_t t = U(2026, 1, 1, 0, 0, 0); t < U(2028, 1, 1, 0, 0, 0); t += 3 * 3600 + 7)
    TEST_ASSERT_TRUE(tz.isDst(t));
}

static void test_parse_invalid(void) {
  static const char* const bad[] = {
      "", ":Europe/Kyiv", "EE-2", "EET", "EET-25", "EET-2:60", "EET-2 ", "<AB>-2", "<A B>-2", "<ABC-2",
      "EET-2EEST,M3.5.0", "EET-2EEST,M13.5.0,M10.5.0", "EET-2EEST,M3.6.0,M10.5.0",
      "EET-2EEST,M3.5.7,M10.5.0", "EET-2EEST,M3.0.0,M10.5.0", "EET-2EEST,J0,J365", "EET-2EEST,366,1",
      "EET-2EEST,M3.5.0/168,M10.5.0", "EET-2EEST,M3.5.0/3,M10.5.0/4x", "EET-2EEST-", "EET-2EE",
      "EET-2EEST,M3.5.0/3;M10.5.0/4", "EET-1234",
  };
  for (const char* s : bad) {
    PosixTz tz;
    TEST_ASSERT_TRUE(tz.parse("EET-2EEST,M3.5.0/3,M10.5.0/4"));
    TEST_ASSERT_FALSE_MESSAGE(tz.parse(s), s);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(0, tz.stdOffset(), s);  // reset to UTC
    TEST_ASSERT_FALSE_MESSAGE(tz.hasDst(), s);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(0, tz.offsetAt(U(2026, 7, 1, 0, 0, 0)), s);
  }
  PosixTz tz;
  TEST_ASSERT_FALSE(tz.parse(nullptr));
}

static void test_random_instants(void) {
  PosixTz zones[tzfx::kRandomZones];
  for (int z = 0; z < tzfx::kRandomZones; ++z) zones[z] = zone(z);
  uint64_t state = tzfx::kSeed;
  int bad = 0, folds = 0;
  char msg[160] = {0};
  for (int i = 0; i < tzfx::kCount; ++i) {
    const int64_t utc = tzfx::kLo + (int64_t)(splitmix64(state) % (uint64_t)tzfx::kSpan);
    const int z = i % tzfx::kRandomZones;
    const PosixTz& tz = zones[z];
    const int32_t exp = tzfx::kOffQ[i] * 900;
    const int32_t got = tz.offsetAt(utc);
    const CivilTime lt = tz.toLocal(utc);
    int64_t back = 0, later = 0;
    const bool ok = tz.toUtc(lt, back, Gap::Reject, Fold::Earlier);
    tz.toUtc(lt, later, Gap::Reject, Fold::Later);
    if (back != later) ++folds;
    const bool good = got == exp && sameCivil(lt, civilOf(utc + exp)) && ok && (back == utc || later == utc) &&
                      tz.isDst(utc) == (tz.hasDst() && exp == tz.dstOffset() && exp != tz.stdOffset());
    if (!good && bad++ == 0)
      snprintf(msg, sizeof msg, "instant %d %s utc=%lld: offset got %ld exp %ld", i, tzfx::kZones[z].name,
               (long long)utc, (long)got, (long)exp);
  }
  if (bad) TEST_FAIL_MESSAGE(msg);
  TEST_ASSERT_TRUE(folds < 10);  // ~1 h / year per zone: a handful at most
}

static void test_transitions(void) {
  int bad = 0;
  char msg[200] = {0};
  for (int i = 0; i < tzfx::kTransitionCount; ++i) {
    const tzfx::Transition& t = tzfx::kTransitions[i];
    const PosixTz tz = zone(t.zone);
    bool good = tz.offsetAt(t.utc - 1) == t.before && tz.offsetAt(t.utc) == t.after;
    int64_t out = 0;
    if (t.after > t.before) {  // gap: local [utc+before, utc+after) does not exist
      const CivilTime first = civilOf(t.utc + t.before);
      const CivilTime last = civilOf(t.utc + t.after - 1);
      good = good && !tz.toUtc(first, out, Gap::Reject) && !tz.toUtc(last, out, Gap::Reject);
      good = good && tz.toUtc(first, out, Gap::AddGap) && out == t.utc;
      good = good && tz.toUtc(civilOf(t.utc + t.before - 1), out, Gap::Reject) && out == t.utc - 1;
      good = good && tz.toUtc(civilOf(t.utc + t.after), out, Gap::Reject) && out == t.utc;
    } else {  // fold: local [utc+after, utc+before) occurs twice
      const int32_t len = t.before - t.after;
      const CivilTime first = civilOf(t.utc + t.after);
      good = good && tz.toUtc(first, out, Gap::Reject, Fold::Earlier) && out == t.utc - len;
      good = good && tz.toUtc(first, out, Gap::Reject, Fold::Later) && out == t.utc;
      const CivilTime past = civilOf(t.utc + t.before);
      good = good && tz.toUtc(past, out, Gap::Reject, Fold::Earlier) && out == t.utc + len;
    }
    if (!good && bad++ == 0)
      snprintf(msg, sizeof msg, "transition %d %s utc=%lld %ld->%ld", i, tzfx::kZones[t.zone].name,
               (long long)t.utc, (long)t.before, (long)t.after);
  }
  if (bad) TEST_FAIL_MESSAGE(msg);
}

static void test_gap_fold_2026(void) {
  PosixTz kyiv;
  TEST_ASSERT_TRUE(kyiv.parse("EET-2EEST,M3.5.0/3,M10.5.0/4"));
  int64_t out = 0;
  // 2026-03-29: 03:00 EET -> 04:00 EEST at 01:00 UTC
  const int64_t spring = U(2026, 3, 29, 1, 0, 0);
  CivilTime lt = kyiv.toLocal(spring - 1);
  TEST_ASSERT_TRUE(sameCivil(CivilTime{2026, 3, 29, 2, 59, 59, 6}, lt));
  lt = kyiv.toLocal(spring);
  TEST_ASSERT_TRUE(sameCivil(CivilTime{2026, 3, 29, 4, 0, 0, 6}, lt));
  TEST_ASSERT_FALSE(kyiv.toUtc(L(2026, 3, 29, 3, 0, 0), out, Gap::Reject));
  TEST_ASSERT_FALSE(kyiv.toUtc(L(2026, 3, 29, 3, 59, 59), out, Gap::Reject));
  TEST_ASSERT_TRUE(kyiv.toUtc(L(2026, 3, 29, 3, 30, 0), out));  // AddGap
  TEST_ASSERT_EQUAL_INT64(spring + 1800, out);
  TEST_ASSERT_TRUE(kyiv.toUtc(L(2026, 3, 29, 4, 0, 0), out, Gap::Reject));
  TEST_ASSERT_EQUAL_INT64(spring, out);
  TEST_ASSERT_TRUE(kyiv.toUtc(L(2026, 3, 29, 2, 59, 59), out, Gap::Reject));
  TEST_ASSERT_EQUAL_INT64(spring - 1, out);
  // 2026-10-25: 04:00 EEST -> 03:00 EET at 01:00 UTC; 03:00..03:59:59 twice
  const int64_t autumn = U(2026, 10, 25, 1, 0, 0);
  TEST_ASSERT_TRUE(kyiv.toUtc(L(2026, 10, 25, 3, 30, 0), out, Gap::Reject, Fold::Earlier));
  TEST_ASSERT_EQUAL_INT64(autumn - 1800, out);
  TEST_ASSERT_TRUE(kyiv.toUtc(L(2026, 10, 25, 3, 30, 0), out, Gap::Reject, Fold::Later));
  TEST_ASSERT_EQUAL_INT64(autumn + 1800, out);
  TEST_ASSERT_TRUE(kyiv.toUtc(L(2026, 10, 25, 3, 0, 0), out, Gap::Reject, Fold::Earlier));
  TEST_ASSERT_EQUAL_INT64(autumn - 3600, out);
  TEST_ASSERT_TRUE(kyiv.toUtc(L(2026, 10, 25, 4, 0, 0), out, Gap::Reject, Fold::Earlier));
  TEST_ASSERT_EQUAL_INT64(autumn + 3600, out);
  TEST_ASSERT_TRUE(kyiv.toUtc(L(2026, 10, 25, 2, 59, 59), out, Gap::Reject, Fold::Later));
  TEST_ASSERT_EQUAL_INT64(autumn - 3600 - 1, out);
  TEST_ASSERT_EQUAL_INT32(10800, kyiv.offsetAt(autumn - 1));
  TEST_ASSERT_EQUAL_INT32(7200, kyiv.offsetAt(autumn));

  PosixTz london;
  TEST_ASSERT_TRUE(london.parse("GMT0BST,M3.5.0/1,M10.5.0"));
  TEST_ASSERT_FALSE(london.toUtc(L(2026, 3, 29, 1, 30, 0), out, Gap::Reject));
  TEST_ASSERT_TRUE(london.toUtc(L(2026, 3, 29, 1, 30, 0), out, Gap::AddGap));
  TEST_ASSERT_EQUAL_INT64(U(2026, 3, 29, 1, 30, 0), out);
  TEST_ASSERT_TRUE(london.toUtc(L(2026, 10, 25, 1, 30, 0), out, Gap::Reject, Fold::Earlier));
  TEST_ASSERT_EQUAL_INT64(U(2026, 10, 25, 0, 30, 0), out);
  TEST_ASSERT_TRUE(london.toUtc(L(2026, 10, 25, 1, 30, 0), out, Gap::Reject, Fold::Later));
  TEST_ASSERT_EQUAL_INT64(U(2026, 10, 25, 1, 30, 0), out);
}

static void test_to_utc_rejects_bad_fields(void) {
  PosixTz tz;
  TEST_ASSERT_TRUE(tz.parse("EET-2EEST,M3.5.0/3,M10.5.0/4"));
  int64_t out = 0;
  TEST_ASSERT_FALSE(tz.toUtc(L(2026, 2, 29, 0, 0, 0), out));
  TEST_ASSERT_TRUE(tz.toUtc(L(2028, 2, 29, 0, 0, 0), out));
  TEST_ASSERT_FALSE(tz.toUtc(L(2026, 13, 1, 0, 0, 0), out));
  TEST_ASSERT_FALSE(tz.toUtc(L(2026, 0, 1, 0, 0, 0), out));
  TEST_ASSERT_FALSE(tz.toUtc(L(2026, 4, 31, 0, 0, 0), out));
  TEST_ASSERT_FALSE(tz.toUtc(L(2026, 1, 1, 24, 0, 0), out));
  TEST_ASSERT_FALSE(tz.toUtc(L(2026, 1, 1, 0, 60, 0), out));
  TEST_ASSERT_FALSE(tz.toUtc(L(2026, 1, 1, 0, 0, 60), out));
  PosixTz utc;  // default: UTC
  TEST_ASSERT_TRUE(utc.toUtc(L(2026, 10, 14, 0, 0, 0), out));
  TEST_ASSERT_EQUAL_INT64(1791936000LL, out);
  TEST_ASSERT_TRUE(sameCivil(CivilTime{2026, 10, 14, 0, 0, 0, 2}, utc.toLocal(1791936000LL)));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_valid);
  RUN_TEST(test_parse_invalid);
  RUN_TEST(test_random_instants);
  RUN_TEST(test_transitions);
  RUN_TEST(test_gap_fold_2026);
  RUN_TEST(test_to_utc_rejects_bad_fields);
  return UNITY_END();
}
