// SES-LIB-006 acceptance 5 (calendar half): monthBounds 2026–2030 vs IANA
// tzdata (scripts/gen_tz_fixture.py), month starts inside a gap / fold,
// dateKey / eveningKey / atLocal, daysInMonth.
#include <stdio.h>
#include <unity.h>

#include <ESPRackPure/Civil.h>
#include <ESPRackPure/LocalCalendar.h>
#include <tz_fixture.h>

using namespace ESPRack;

void setUp(void) {}
void tearDown(void) {}

static int64_t U(int32_t y, uint32_t mo, uint32_t d, uint32_t h, uint32_t mi, uint32_t s) {
  return civil::utcFromCivil(y, mo, d, h, mi, s);
}

static void test_month_bounds_fixture(void) {
  int bad = 0, checked = 0;
  char msg[160] = {0};
  for (int i = 0; i < tzfx::kMonthStartCount; ++i) {
    const tzfx::MonthStart& ms = tzfx::kMonthStarts[i];
    PosixTz tz;
    TEST_ASSERT_TRUE(tz.parse(tzfx::kZones[ms.zone].posix));
    const bool hasNext = i + 1 < tzfx::kMonthStartCount && tzfx::kMonthStarts[i + 1].zone == ms.zone;
    const int64_t next = hasNext ? tzfx::kMonthStarts[i + 1].utc : 0;
    const uint32_t key = (uint32_t)ms.y * 100u + ms.m;
    int64_t m0 = 0, m1 = 0;
    bool good = true;
    // first second, middle and last second of the month all map to it
    const int64_t probes[3] = {ms.utc, ms.utc + 14 * 86400 + 4321, hasNext ? next - 1 : ms.utc + 27 * 86400};
    for (int64_t t : probes) {
      cal::monthBounds(tz, t, m0, m1);
      good = good && m0 == ms.utc && (!hasNext || m1 == next) && cal::monthKey(tz, t) == key;
    }
    const uint32_t prevKey = ms.m == 1 ? (uint32_t)(ms.y - 1) * 100u + 12u : key - 1u;
    good = good && cal::monthKey(tz, ms.utc - 1) == prevKey;
    good = good && cal::atLocal(tz, key * 100u + 1u, 0) == ms.utc;
    ++checked;
    if (!good && bad++ == 0)
      snprintf(msg, sizeof msg, "%s %d-%02d: m0 %lld exp %lld, m1 %lld exp %lld", tzfx::kZones[ms.zone].name,
               (int)ms.y, (int)ms.m, (long long)m0, (long long)ms.utc, (long long)m1, (long long)next);
  }
  if (bad) TEST_FAIL_MESSAGE(msg);
  TEST_ASSERT_EQUAL_INT(tzfx::kZoneCount * 60, checked);
}

static void test_month_start_in_gap_and_fold(void) {
  // std -3 h, DST -2 h; DST starts 1st Sunday of April at 00:00 (gap 00:00..01:00)
  // and ends 1st Sunday of October at 01:00 DST (00:00..01:00 occurs twice).
  PosixTz tz;
  TEST_ASSERT_TRUE(tz.parse("AAA3BBB,M4.1.0/0,M10.1.0/1"));
  TEST_ASSERT_EQUAL_UINT8(6, civil::weekdayMon0(civil::daysFromCivil(2029, 4, 1)));
  TEST_ASSERT_EQUAL_UINT8(6, civil::weekdayMon0(civil::daysFromCivil(2028, 10, 1)));
  int64_t m0 = 0, m1 = 0;
  // April 2029 starts in the gap: shifted forward to 01:00 DST = 03:00 UTC
  cal::monthBounds(tz, U(2029, 4, 15, 12, 0, 0), m0, m1);
  TEST_ASSERT_EQUAL_INT64(U(2029, 4, 1, 3, 0, 0), m0);
  cal::monthBounds(tz, U(2029, 3, 15, 12, 0, 0), m0, m1);
  TEST_ASSERT_EQUAL_INT64(U(2029, 4, 1, 3, 0, 0), m1);
  TEST_ASSERT_EQUAL_UINT32(202904u, cal::monthKey(tz, U(2029, 4, 1, 3, 0, 0)));
  TEST_ASSERT_EQUAL_UINT32(202903u, cal::monthKey(tz, U(2029, 4, 1, 3, 0, 0) - 1));
  // October 2028 starts in the fold: the earlier instant, 00:00 DST = 02:00 UTC
  cal::monthBounds(tz, U(2028, 10, 15, 12, 0, 0), m0, m1);
  TEST_ASSERT_EQUAL_INT64(U(2028, 10, 1, 2, 0, 0), m0);
  TEST_ASSERT_EQUAL_INT64(U(2028, 11, 1, 3, 0, 0), m1);
  TEST_ASSERT_EQUAL_UINT32(202809u, cal::monthKey(tz, U(2028, 10, 1, 2, 0, 0) - 1));
  TEST_ASSERT_EQUAL_UINT32(202810u, cal::monthKey(tz, U(2028, 10, 1, 3, 30, 0)));  // the repeat
}

static void test_date_and_evening_keys(void) {
  PosixTz kyiv;
  TEST_ASSERT_TRUE(kyiv.parse("EET-2EEST,M3.5.0/3,M10.5.0/4"));
  const int64_t t = U(2026, 10, 14, 2, 0, 0);  // 05:00 EEST
  TEST_ASSERT_EQUAL_UINT32(20261014u, cal::dateKey(kyiv, t));
  TEST_ASSERT_EQUAL_UINT32(20261013u, cal::eveningKey(kyiv, t));
  TEST_ASSERT_EQUAL_UINT32(20261014u, cal::eveningKey(kyiv, U(2026, 10, 14, 9, 0, 0)));  // 12:00 local
  TEST_ASSERT_EQUAL_UINT32(20261013u, cal::eveningKey(kyiv, U(2026, 10, 14, 9, 0, 0) - 1));
  TEST_ASSERT_EQUAL_UINT32(20261013u, cal::dateKey(kyiv, U(2026, 10, 13, 20, 59, 59)));
  TEST_ASSERT_EQUAL_UINT32(20261014u, cal::dateKey(kyiv, U(2026, 10, 13, 21, 0, 0)));
  TEST_ASSERT_EQUAL_UINT32(20270101u, cal::dateKey(kyiv, U(2026, 12, 31, 22, 0, 0)));
  TEST_ASSERT_EQUAL_UINT32(20261231u, cal::eveningKey(kyiv, U(2026, 12, 31, 22, 0, 0)));
}

static void test_at_local(void) {
  PosixTz kyiv;
  TEST_ASSERT_TRUE(kyiv.parse("EET-2EEST,M3.5.0/3,M10.5.0/4"));
  TEST_ASSERT_EQUAL_INT64(U(2026, 10, 14, 15, 0, 0), cal::atLocal(kyiv, 20261014u, 18 * 3600));
  TEST_ASSERT_EQUAL_INT64(U(2026, 10, 14, 22, 0, 0), cal::atLocal(kyiv, 20261014u, 86400 + 3600));
  TEST_ASSERT_EQUAL_INT64(U(2027, 1, 1, 4, 0, 0), cal::atLocal(kyiv, 20261231u, 86400 + 6 * 3600));
  TEST_ASSERT_EQUAL_INT64(cal::kNoTime, cal::atLocal(kyiv, 20260230u, 0));
  TEST_ASSERT_EQUAL_INT64(cal::kNoTime, cal::atLocal(kyiv, 20261300u, 0));
  TEST_ASSERT_EQUAL_INT64(cal::kNoTime, cal::atLocal(kyiv, 20261000u, 0));
  // gap: forward; fold: as asked
  TEST_ASSERT_EQUAL_INT64(U(2026, 3, 29, 1, 30, 0), cal::atLocal(kyiv, 20260329u, 3 * 3600 + 1800));
  TEST_ASSERT_EQUAL_INT64(U(2026, 10, 25, 0, 30, 0),
                          cal::atLocal(kyiv, 20261025u, 3 * 3600 + 1800, PosixTz::Fold::Earlier));
  TEST_ASSERT_EQUAL_INT64(U(2026, 10, 25, 1, 30, 0),
                          cal::atLocal(kyiv, 20261025u, 3 * 3600 + 1800, PosixTz::Fold::Later));
  // the DST-night length change lives in the labels: 18:00 -> 06:00 spans 13 h / 11 h
  TEST_ASSERT_EQUAL_INT64(13 * 3600, cal::atLocal(kyiv, 20261024u, 86400 + 6 * 3600) -
                                         cal::atLocal(kyiv, 20261024u, 18 * 3600));
  TEST_ASSERT_EQUAL_INT64(11 * 3600, cal::atLocal(kyiv, 20260328u, 86400 + 6 * 3600) -
                                         cal::atLocal(kyiv, 20260328u, 18 * 3600));
}

static void test_days_in_month(void) {
  TEST_ASSERT_EQUAL_UINT8(29, cal::daysInMonth(2028, 2));
  TEST_ASSERT_EQUAL_UINT8(28, cal::daysInMonth(2026, 2));
  TEST_ASSERT_EQUAL_UINT8(28, cal::daysInMonth(2100, 2));
  TEST_ASSERT_EQUAL_UINT8(29, cal::daysInMonth(2000, 2));
  TEST_ASSERT_EQUAL_UINT8(30, cal::daysInMonth(2026, 4));
  TEST_ASSERT_EQUAL_UINT8(31, cal::daysInMonth(2026, 12));
  TEST_ASSERT_EQUAL_UINT8(0, cal::daysInMonth(2026, 0));
  TEST_ASSERT_EQUAL_UINT8(0, cal::daysInMonth(2026, 13));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_month_bounds_fixture);
  RUN_TEST(test_month_start_in_gap_and_fold);
  RUN_TEST(test_date_and_evening_keys);
  RUN_TEST(test_at_local);
  RUN_TEST(test_days_in_month);
  return UNITY_END();
}
