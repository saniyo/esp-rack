// SES-LIB-006 acceptance 1: civil round-trip 1970–2100, epochFromBuildDate.
#include <unity.h>

#include <ESPRackPure/Civil.h>

using namespace ESPRack;

void setUp(void) {}
void tearDown(void) {}

static void test_known_days(void) {
  TEST_ASSERT_EQUAL_INT64(0, civil::daysFromCivil(1970, 1, 1));
  TEST_ASSERT_EQUAL_INT64(-1, civil::daysFromCivil(1969, 12, 31));
  TEST_ASSERT_EQUAL_INT64(10957, civil::daysFromCivil(2000, 1, 1));
  TEST_ASSERT_EQUAL_INT64(11016, civil::daysFromCivil(2000, 2, 29));   // leap century
  TEST_ASSERT_EQUAL_INT64(47482, civil::daysFromCivil(2100, 1, 1));
  TEST_ASSERT_EQUAL_INT64(1767225600LL, civil::utcFromCivil(2026, 1, 1, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT64(1791936000LL, civil::utcFromCivil(2026, 10, 14, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT64(4102444800LL, civil::utcFromCivil(2100, 1, 1, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT64(951782400LL + 3600 + 120 + 3, civil::utcFromCivil(2000, 2, 29, 1, 2, 3));
}

static void test_round_trip_1970_2100(void) {
  int32_t py = 1969;
  uint32_t pm = 12, pd = 31;
  const int64_t last = civil::daysFromCivil(2100, 12, 31);
  for (int64_t z = 0; z <= last; ++z) {
    int32_t y;
    uint32_t m, d;
    civil::civilFromDays(z, y, m, d);
    TEST_ASSERT_EQUAL_INT64(z, civil::daysFromCivil(y, m, d));
    // consecutive days advance by exactly one civil day
    const bool sameMonth = (y == py && m == pm && d == pd + 1);
    const bool newMonth = (d == 1 && ((y == py && m == pm + 1) || (y == py + 1 && m == 1 && pm == 12)));
    TEST_ASSERT_TRUE_MESSAGE(sameMonth || newMonth, "civil day sequence broken");
    py = y; pm = m; pd = d;
  }
  TEST_ASSERT_EQUAL_INT(2100, py);
  TEST_ASSERT_EQUAL_UINT(12, pm);
  TEST_ASSERT_EQUAL_UINT(31, pd);
}

static void test_negative_days(void) {
  int32_t y;
  uint32_t m, d;
  civil::civilFromDays(-719468, y, m, d);   // 0000-03-01
  TEST_ASSERT_EQUAL_INT(0, y);
  TEST_ASSERT_EQUAL_UINT(3, m);
  TEST_ASSERT_EQUAL_UINT(1, d);
}

static void test_weekday(void) {
  TEST_ASSERT_EQUAL_UINT8(3, civil::weekdayMon0(0));                                   // 1970-01-01 Thu
  TEST_ASSERT_EQUAL_UINT8(2, civil::weekdayMon0(-1));                                  // 1969-12-31 Wed
  TEST_ASSERT_EQUAL_UINT8(1, civil::weekdayMon0(civil::daysFromCivil(2026, 10, 13)));  // Tue
  TEST_ASSERT_EQUAL_UINT8(6, civil::weekdayMon0(civil::daysFromCivil(2026, 3, 29)));   // Sun
  TEST_ASSERT_EQUAL_UINT8(5, civil::weekdayMon0(civil::daysFromCivil(2000, 1, 1)));    // Sat
}

static void test_build_date(void) {
  TEST_ASSERT_EQUAL_INT64(civil::utcFromCivil(2026, 10, 13, 0, 0, 0), civil::epochFromBuildDate("Oct 13 2026"));
  TEST_ASSERT_EQUAL_INT64(civil::utcFromCivil(2026, 3, 3, 0, 0, 0), civil::epochFromBuildDate("Mar  3 2026"));
  TEST_ASSERT_EQUAL_INT64(0, civil::epochFromBuildDate("Foo 13 2026"));
  TEST_ASSERT_EQUAL_INT64(0, civil::epochFromBuildDate("Oct 1x 2026"));
  TEST_ASSERT_EQUAL_INT64(0, civil::epochFromBuildDate(nullptr));
  static_assert(civil::epochFromBuildDate("Jan  1 2026") == 1767225600LL, "constexpr build date");
  TEST_ASSERT_TRUE(civil::epochFromBuildDate(__DATE__) > 1767225600LL);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_known_days);
  RUN_TEST(test_round_trip_1970_2100);
  RUN_TEST(test_negative_days);
  RUN_TEST(test_weekday);
  RUN_TEST(test_build_date);
  return UNITY_END();
}
