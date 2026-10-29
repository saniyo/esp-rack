// Host tests of App::loop's wait arithmetic (system/LoopWait.h): timers,
// the bound, the watchdog cap and the ms -> tick rounding.

#include <unity.h>

#include <system/LoopWait.h>

using namespace ESPRack;

void setUp() {}
void tearDown() {}

void test_due_in() {
  TEST_ASSERT_EQUAL_UINT32(10000, LoopWait::dueIn(5000, 5000, 10000));
  TEST_ASSERT_EQUAL_UINT32(1, LoopWait::dueIn(14999, 5000, 10000));
  TEST_ASSERT_EQUAL_UINT32(0, LoopWait::dueIn(15000, 5000, 10000));
  TEST_ASSERT_EQUAL_UINT32(0, LoopWait::dueIn(90000, 5000, 10000));  // overdue
  TEST_ASSERT_EQUAL_UINT32(0, LoopWait::dueIn(7, 7, 0));             // no period: always due
}

void test_due_in_wraps() {
  // millis() wraps after 49.7 days: same answer as `now - since >= period`.
  const uint32_t since = 0xFFFFF000u;
  TEST_ASSERT_EQUAL_UINT32(10000 - 0x1000, LoopWait::dueIn(0, since, 10000));
  TEST_ASSERT_EQUAL_UINT32(0, LoopWait::dueIn(since + 10000, since, 10000));
}

void test_bound() {
  TEST_ASSERT_EQUAL_UINT32(10, LoopWait::bound(10, 1000, 0));
  TEST_ASSERT_EQUAL_UINT32(1000, LoopWait::bound(120000, 1000, 0));  // a far timer
  TEST_ASSERT_EQUAL_UINT32(0, LoopWait::bound(0, 1000, 0));
  // watchdog on: half its timeout at most
  TEST_ASSERT_EQUAL_UINT32(1000, LoopWait::bound(5000, 1000, 5000));
  TEST_ASSERT_EQUAL_UINT32(400, LoopWait::bound(5000, 1000, 800));
  TEST_ASSERT_EQUAL_UINT32(10, LoopWait::bound(10, 1000, 800));
}

void test_ticks() {
  TEST_ASSERT_EQUAL_UINT32(0, LoopWait::ticksFor(0, 1000));
  TEST_ASSERT_EQUAL_UINT32(10, LoopWait::ticksFor(10, 1000));
  TEST_ASSERT_EQUAL_UINT32(1, LoopWait::ticksFor(1, 100));   // never 0 for a wait: no spin
  TEST_ASSERT_EQUAL_UINT32(1, LoopWait::ticksFor(10, 100));
  TEST_ASSERT_EQUAL_UINT32(2, LoopWait::ticksFor(11, 100));  // rounded up
  TEST_ASSERT_EQUAL_UINT32(100, LoopWait::ticksFor(1000, 100));
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFEu, LoopWait::ticksFor(0xFFFFFFFFu, 1000000));  // never portMAX_DELAY
}

void test_defaults() {
  TEST_ASSERT_EQUAL_UINT32(10, ESPRACK_LOOP_DEFAULT_WAIT_MS);
  TEST_ASSERT_EQUAL_UINT32(1000, ESPRACK_LOOP_MAX_WAIT_MS);
  TEST_ASSERT_TRUE(ESPRACK_LOOP_IDLE_WAIT_MS >= ESPRACK_LOOP_DEFAULT_WAIT_MS);
  TEST_ASSERT_TRUE(ESPRACK_LOOP_IDLE_WAIT_MS <= ESPRACK_LOOP_MAX_WAIT_MS);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_due_in);
  RUN_TEST(test_due_in_wraps);
  RUN_TEST(test_bound);
  RUN_TEST(test_ticks);
  RUN_TEST(test_defaults);
  return UNITY_END();
}
