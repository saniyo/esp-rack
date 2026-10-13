// SES-LIB-006 acceptance 5 (window half): intersect / overlap / clip and the
// forEvening endpoint rules — evening pivot, day mask on the evening date,
// from = Fold::Earlier, to = Fold::Later, gaps crossed forward, DST nights.
#include <unity.h>

#include <ESPRackPure/Civil.h>
#include <ESPRackPure/TimeWindow.h>

using namespace ESPRack;
using tw::DailyWindow;
using tw::Interval;

void setUp(void) {}
void tearDown(void) {}

static constexpr uint8_t ALL = 0x7F;
static int64_t U(int32_t y, uint32_t mo, uint32_t d, uint32_t h, uint32_t mi) {
  return civil::utcFromCivil(y, mo, d, h, mi, 0);
}
static PosixTz kyiv() {
  PosixTz tz;
  TEST_ASSERT_TRUE(tz.parse("EET-2EEST,M3.5.0/3,M10.5.0/4"));
  return tz;
}
static uint16_t hm(int h, int m) { return (uint16_t)(h * 60 + m); }
#define ASSERT_IV(ea, eb, iv)            \
  do {                                   \
    TEST_ASSERT_EQUAL_INT64((ea), (iv).a); \
    TEST_ASSERT_EQUAL_INT64((eb), (iv).b); \
  } while (0)

static void test_intersect_overlap_clip(void) {
  ASSERT_IV(5, 10, tw::intersect(Interval{0, 10}, Interval{5, 20}));
  ASSERT_IV(20, 20, tw::intersect(Interval{0, 10}, Interval{20, 30}));  // empty
  TEST_ASSERT_EQUAL_INT64(5, tw::overlap(Interval{0, 10}, Interval{5, 20}));
  TEST_ASSERT_EQUAL_INT64(0, tw::overlap(Interval{0, 10}, Interval{10, 20}));  // half-open
  TEST_ASSERT_EQUAL_INT64(0, tw::overlap(Interval{0, 10}, Interval{20, 30}));
  TEST_ASSERT_EQUAL_INT64(0, tw::overlap(Interval{5, 5}, Interval{0, 10}));
  Interval v[5] = {{0, 10}, {15, 25}, {30, 40}, {45, 50}, {8, 60}};
  const size_t n = tw::clip(v, 5, 20, 46);
  TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)n);
  ASSERT_IV(20, 25, v[0]);
  ASSERT_IV(30, 40, v[1]);
  ASSERT_IV(45, 46, v[2]);
  ASSERT_IV(20, 46, v[3]);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)tw::clip(v, 4, 100, 200));
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)tw::clip(nullptr, 0, 0, 1));
}

static void test_evening_pivot(void) {
  const PosixTz tz = kyiv();
  Interval iv{};
  // 18:00 -> 06:00 for the evening of Wed 2026-10-14 (EEST, +3)
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(18, 0), hm(6, 0), ALL}, 20261014u, iv));
  ASSERT_IV(U(2026, 10, 14, 15, 0), U(2026, 10, 15, 3, 0), iv);
  // from < 12:00 belongs to the previous evening: 02:00-05:00 is on the 15th
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(2, 0), hm(5, 0), ALL}, 20261014u, iv));
  ASSERT_IV(U(2026, 10, 14, 23, 0), U(2026, 10, 15, 2, 0), iv);
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(11, 59), hm(13, 0), ALL}, 20261014u, iv));
  ASSERT_IV(U(2026, 10, 15, 8, 59), U(2026, 10, 15, 10, 0), iv);
  // from = 12:00 is the evening date itself
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(12, 0), hm(13, 0), ALL}, 20261014u, iv));
  ASSERT_IV(U(2026, 10, 14, 9, 0), U(2026, 10, 14, 10, 0), iv);
  // to = 24:00, and from == to is a full day
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(18, 0), 1440, ALL}, 20261014u, iv));
  ASSERT_IV(U(2026, 10, 14, 15, 0), U(2026, 10, 14, 21, 0), iv);
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(18, 0), hm(18, 0), ALL}, 20261014u, iv));
  ASSERT_IV(U(2026, 10, 14, 15, 0), U(2026, 10, 15, 15, 0), iv);
  // an after-midnight slot that wraps itself ends two dates later
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(2, 0), hm(1, 0), ALL}, 20261014u, iv));
  ASSERT_IV(U(2026, 10, 14, 23, 0), U(2026, 10, 15, 22, 0), iv);
}

static void test_day_mask_on_evening_date(void) {
  const PosixTz tz = kyiv();
  Interval iv{};
  TEST_ASSERT_EQUAL_UINT8(2, civil::weekdayMon0(civil::daysFromCivil(2026, 10, 14)));  // Wednesday
  const uint8_t wedOnly = 1u << 2, thuOnly = 1u << 3;
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(18, 0), hm(6, 0), wedOnly}, 20261014u, iv));
  TEST_ASSERT_FALSE(tw::forEvening(tz, DailyWindow{hm(18, 0), hm(6, 0), thuOnly}, 20261014u, iv));
  // the 02:00 slot runs on Thursday's date but belongs to Wednesday evening
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(2, 0), hm(5, 0), wedOnly}, 20261014u, iv));
  TEST_ASSERT_FALSE(tw::forEvening(tz, DailyWindow{hm(2, 0), hm(5, 0), thuOnly}, 20261014u, iv));
  TEST_ASSERT_FALSE(tw::forEvening(tz, DailyWindow{hm(18, 0), hm(6, 0), 0}, 20261014u, iv));
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(18, 0), hm(6, 0), 1u << 6}, 20261018u, iv));  // Sunday
}

static void test_dst_nights_and_endpoints(void) {
  const PosixTz tz = kyiv();
  Interval iv{};
  // schedule length changes by exactly +-3600 s across a DST night
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(18, 0), hm(6, 0), ALL}, 20261024u, iv));
  TEST_ASSERT_EQUAL_INT64(13 * 3600, iv.b - iv.a);
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(18, 0), hm(6, 0), ALL}, 20260328u, iv));
  TEST_ASSERT_EQUAL_INT64(11 * 3600, iv.b - iv.a);
  // fold (03:00-03:59 twice on 2026-10-25): from -> earlier, to -> later
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(22, 0), hm(3, 30), ALL}, 20261024u, iv));
  ASSERT_IV(U(2026, 10, 24, 19, 0), U(2026, 10, 25, 1, 30), iv);
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(3, 30), hm(5, 0), ALL}, 20261024u, iv));
  ASSERT_IV(U(2026, 10, 25, 0, 30), U(2026, 10, 25, 3, 0), iv);
  // gap (03:00-03:59 missing on 2026-03-29): endpoints move forward by the gap
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(22, 0), hm(3, 30), ALL}, 20260328u, iv));
  ASSERT_IV(U(2026, 3, 28, 20, 0), U(2026, 3, 29, 1, 30), iv);
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(3, 30), hm(5, 0), ALL}, 20260328u, iv));
  ASSERT_IV(U(2026, 3, 29, 1, 30), U(2026, 3, 29, 2, 0), iv);
  // a window entirely inside the gap moves forward as a whole (04:10-04:20 EEST)
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(3, 10), hm(3, 20), ALL}, 20260328u, iv));
  ASSERT_IV(U(2026, 3, 29, 1, 10), U(2026, 3, 29, 1, 20), iv);
  // ... and one that starts before it keeps its physical length
  TEST_ASSERT_TRUE(tw::forEvening(tz, DailyWindow{hm(2, 30), hm(3, 30), ALL}, 20260328u, iv));
  ASSERT_IV(U(2026, 3, 29, 0, 30), U(2026, 3, 29, 1, 30), iv);
}

static void test_invalid(void) {
  const PosixTz tz = kyiv();
  Interval iv{7, 8};
  TEST_ASSERT_FALSE(tw::forEvening(tz, DailyWindow{1440, hm(6, 0), ALL}, 20261014u, iv));
  TEST_ASSERT_FALSE(tw::forEvening(tz, DailyWindow{hm(18, 0), 1441, ALL}, 20261014u, iv));
  TEST_ASSERT_FALSE(tw::forEvening(tz, DailyWindow{hm(18, 0), hm(6, 0), ALL}, 20260230u, iv));
  TEST_ASSERT_FALSE(tw::forEvening(tz, DailyWindow{hm(18, 0), hm(6, 0), ALL}, 20261300u, iv));
  ASSERT_IV(7, 8, iv);  // untouched on failure
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_intersect_overlap_clip);
  RUN_TEST(test_evening_pivot);
  RUN_TEST(test_day_mask_on_evening_date);
  RUN_TEST(test_dst_nights_and_endpoints);
  RUN_TEST(test_invalid);
  return UNITY_END();
}
