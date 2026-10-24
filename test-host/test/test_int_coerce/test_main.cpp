// IntCoerce: the integer rules FormBuilder::updateValue uses
// for long / unsigned long / long long / unsigned long long — i.e. int32_t,
// uint32_t, int64_t, uint64_t on ESP32.
#include <unity.h>

#include <stdint.h>

#include <limits>

#include <web/IntCoerce.h>

using namespace web::coerce;

void setUp(void) {}
void tearDown(void) {}

// Number fields that round a double (uint8_t, uint16_t, int, float, double):
// a cleared field ("") or text is not a value, so it can't become 0.
static void test_number_from_string(void) {
  double d = -1;
  TEST_ASSERT_TRUE(numberFromString("9.00", d));
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 9.0, d);
  TEST_ASSERT_TRUE(numberFromString(" -1.5 ", d));
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, -1.5, d);
  TEST_ASSERT_TRUE(numberFromString("1e3", d));
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1000.0, d);
  TEST_ASSERT_TRUE(numberFromString("3.3", d));
  TEST_ASSERT_TRUE(d == 3.3);  // exactly the double the literal gives
  TEST_ASSERT_TRUE(numberFromString("+0.25e-2", d));
  TEST_ASSERT_DOUBLE_WITHIN(1e-15, 0.0025, d);
  TEST_ASSERT_TRUE(numberFromString("4200", d));
  TEST_ASSERT_TRUE(d == 4200.0);
  TEST_ASSERT_FALSE(numberFromString(".", d));
  TEST_ASSERT_FALSE(numberFromString("-", d));
  TEST_ASSERT_FALSE(numberFromString("1e", d));
  d = 7;
  TEST_ASSERT_FALSE(numberFromString("", d));
  TEST_ASSERT_FALSE(numberFromString("   ", d));
  TEST_ASSERT_FALSE(numberFromString(nullptr, d));
  TEST_ASSERT_FALSE(numberFromString("abc", d));
  TEST_ASSERT_FALSE(numberFromString("12abc", d));
  TEST_ASSERT_FALSE(numberFromString("1.2.3", d));
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 7.0, d);  // untouched
}

static void test_double_rounds_and_clamps(void) {
  TEST_ASSERT_EQUAL_INT64(42, fromDouble<int32_t>(42.0));
  TEST_ASSERT_EQUAL_INT64(43, fromDouble<int32_t>(42.5));
  TEST_ASSERT_EQUAL_INT64(-43, fromDouble<int32_t>(-42.5));
  TEST_ASSERT_EQUAL_INT64(-42, fromDouble<int32_t>(-42.4));
  TEST_ASSERT_EQUAL_INT64(INT32_MAX, fromDouble<int32_t>(5e9));
  TEST_ASSERT_EQUAL_INT64(INT32_MAX, fromDouble<int32_t>(2147483646.7));
  TEST_ASSERT_EQUAL_INT64(INT32_MIN, fromDouble<int32_t>(-5e9));
  TEST_ASSERT_EQUAL_UINT64(0, fromDouble<uint32_t>(-1.0));
  TEST_ASSERT_EQUAL_UINT64(0, fromDouble<uint32_t>(0.4));
  TEST_ASSERT_EQUAL_UINT64(UINT32_MAX, fromDouble<uint32_t>(1e12));
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, fromDouble<int64_t>(1e30));
  TEST_ASSERT_EQUAL_INT64(INT64_MIN, fromDouble<int64_t>(-1e30));
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, fromDouble<uint64_t>(1e30));
  const double nan = std::numeric_limits<double>::quiet_NaN();
  TEST_ASSERT_EQUAL_INT64(0, fromDouble<int32_t>(nan));
}

static void test_string(void) {
  int32_t i = 7;
  TEST_ASSERT_TRUE(fromString<int32_t>("42", i));
  TEST_ASSERT_EQUAL_INT64(42, i);
  TEST_ASSERT_TRUE(fromString<int32_t>("  -17 ", i));
  TEST_ASSERT_EQUAL_INT64(-17, i);
  TEST_ASSERT_TRUE(fromString<int32_t>("+5", i));
  TEST_ASSERT_EQUAL_INT64(5, i);
  TEST_ASSERT_TRUE(fromString<int32_t>("99999999999", i));
  TEST_ASSERT_EQUAL_INT64(INT32_MAX, i);
  TEST_ASSERT_TRUE(fromString<int32_t>("-99999999999", i));
  TEST_ASSERT_EQUAL_INT64(INT32_MIN, i);
  TEST_ASSERT_TRUE(fromString<int32_t>("-999999999999999999999999", i));
  TEST_ASSERT_EQUAL_INT64(INT32_MIN, i);

  uint32_t u = 7;
  TEST_ASSERT_TRUE(fromString<uint32_t>("-3", u));
  TEST_ASSERT_EQUAL_UINT64(0, u);
  TEST_ASSERT_TRUE(fromString<uint32_t>("4294967296", u));
  TEST_ASSERT_EQUAL_UINT64(UINT32_MAX, u);

  uint64_t u64 = 0;
  TEST_ASSERT_TRUE(fromString<uint64_t>("18446744073709551615", u64));
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, u64);
  TEST_ASSERT_TRUE(fromString<uint64_t>("99999999999999999999999", u64));
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, u64);
  int64_t i64 = 0;
  TEST_ASSERT_TRUE(fromString<int64_t>("-9223372036854775808", i64));
  TEST_ASSERT_TRUE(i64 == INT64_MIN);
}

static void test_string_not_a_number_is_ignored(void) {
  int32_t i = 7;
  TEST_ASSERT_FALSE(fromString<int32_t>("abc", i));
  TEST_ASSERT_FALSE(fromString<int32_t>("12abc", i));
  TEST_ASSERT_FALSE(fromString<int32_t>("1.5", i));
  TEST_ASSERT_FALSE(fromString<int32_t>("", i));
  TEST_ASSERT_FALSE(fromString<int32_t>("-", i));
  TEST_ASSERT_FALSE(fromString<int32_t>(nullptr, i));
  TEST_ASSERT_EQUAL_INT64(7, i);  // untouched
}

static void test_integers(void) {
  TEST_ASSERT_EQUAL_INT64(-5, fromSigned<int32_t>(-5));
  TEST_ASSERT_EQUAL_INT64(INT32_MAX, fromSigned<int32_t>(5000000000LL));
  TEST_ASSERT_EQUAL_INT64(INT32_MIN, fromSigned<int32_t>(-5000000000LL));
  TEST_ASSERT_EQUAL_UINT64(0, fromSigned<uint32_t>(-1));
  TEST_ASSERT_EQUAL_UINT64(UINT32_MAX, fromSigned<uint32_t>(5000000000LL));
  TEST_ASSERT_EQUAL_UINT64(5000000000ULL, fromSigned<uint64_t>(5000000000LL));
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, fromUnsigned<int64_t>(UINT64_MAX));
  TEST_ASSERT_EQUAL_UINT64(UINT32_MAX, fromUnsigned<uint32_t>(1ULL << 40));
  TEST_ASSERT_EQUAL_UINT64(12, fromUnsigned<uint32_t>(12));
}

static void test_long_types(void) {
  // the overload set on ESP32: long, unsigned long, long long, unsigned long long
  long l = 0;
  TEST_ASSERT_TRUE(fromString<long>("123", l));
  TEST_ASSERT_EQUAL_INT64(123, l);
  unsigned long ul = 0;
  TEST_ASSERT_TRUE(fromString<unsigned long>("-1", ul));
  TEST_ASSERT_EQUAL_UINT64(0, ul);
  TEST_ASSERT_EQUAL_INT64(std::numeric_limits<long>::max(), fromDouble<long>(1e30));
  long long ll = 0;
  TEST_ASSERT_TRUE(fromString<long long>("-42", ll));
  TEST_ASSERT_EQUAL_INT64(-42, ll);
  TEST_ASSERT_EQUAL_UINT64(std::numeric_limits<unsigned long long>::max(), fromDouble<unsigned long long>(1e30));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_double_rounds_and_clamps);
  RUN_TEST(test_number_from_string);
  RUN_TEST(test_string);
  RUN_TEST(test_string_not_a_number_is_ignored);
  RUN_TEST(test_integers);
  RUN_TEST(test_long_types);
  return UNITY_END();
}
