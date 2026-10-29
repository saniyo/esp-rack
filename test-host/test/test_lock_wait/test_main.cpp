// LockWait: the loop-wait deadline math, the log throttle and PinTable
// (WsManager's client liveness: a disconnect waits for sends on other tasks).
#include <unity.h>

#include <stdint.h>

#include <LockWait.h>

using ESPRack::PinTable;

void setUp(void) {}
void tearDown(void) {}

static void test_ms_until_due(void) {
  TEST_ASSERT_EQUAL(5000, ESPRack::msUntilDue(1000, 5000, 1000));
  TEST_ASSERT_EQUAL(1, ESPRack::msUntilDue(1000, 5000, 5999));
  TEST_ASSERT_EQUAL(0, ESPRack::msUntilDue(1000, 5000, 6000));
  TEST_ASSERT_EQUAL(0, ESPRack::msUntilDue(1000, 5000, 90000));  // overdue
  // Across the millis() wrap.
  TEST_ASSERT_EQUAL(3000, ESPRack::msUntilDue(0xFFFFF000u, 5000, 0xFFFFF000u + 2000u));
  TEST_ASSERT_EQUAL(0, ESPRack::msUntilDue(0xFFFFF000u, 5000, 904u));  // 0x1000 + 904 = 5000 later
  TEST_ASSERT_EQUAL(0, ESPRack::msUntilDue(7, 0, 7));                 // no period: always due
}

static void test_log_this_fail(void) {
  TEST_ASSERT_FALSE(ESPRack::logThisFail(0));
  TEST_ASSERT_TRUE(ESPRack::logThisFail(1));
  TEST_ASSERT_TRUE(ESPRack::logThisFail(2));
  TEST_ASSERT_FALSE(ESPRack::logThisFail(3));
  TEST_ASSERT_TRUE(ESPRack::logThisFail(4));
  TEST_ASSERT_FALSE(ESPRack::logThisFail(1000));
  TEST_ASSERT_TRUE(ESPRack::logThisFail(1024));
}

static int a_obj, b_obj, c_obj;
static int loopTask, asyncTask, otherTask;  // three distinct addresses
static const void* const LOOP = &loopTask;
static const void* const ASYNC = &asyncTask;
static const void* const OTHER = &otherTask;

static void test_pin_basic(void) {
  PinTable<int, 2> t;
  TEST_ASSERT_FALSE(t.add(0, &a_obj));        // id 0 is "none"
  TEST_ASSERT_TRUE(t.add(7, &a_obj));
  TEST_ASSERT_TRUE(t.add(8, &b_obj));
  TEST_ASSERT_FALSE(t.add(9, &c_obj));        // full
  TEST_ASSERT_TRUE(t.pin(7, LOOP) == &a_obj);
  TEST_ASSERT_TRUE(t.pin(99, LOOP) == nullptr);
  TEST_ASSERT_TRUE(t.peek(8) == &b_obj);
  uint32_t ids[4];
  TEST_ASSERT_EQUAL(2, (int)t.ids(ids, 4));
  TEST_ASSERT_EQUAL(1, (int)t.ids(ids, 1));
  int slot = -1;
  TEST_ASSERT_FALSE(t.unpin(7, LOOP, slot));  // nobody closing
  // Closing with no pins: gone at once, the slot is free again.
  TEST_ASSERT_EQUAL(-1, t.close(7, ASYNC));
  TEST_ASSERT_FALSE(t.has(7));
  TEST_ASSERT_TRUE(t.add(9, &c_obj));
  TEST_ASSERT_EQUAL(-1, t.close(12345, ASYNC));  // unknown
}

// The disconnect (async_tcp) waits while the loop task sends to the client.
static void test_close_waits_for_other_task(void) {
  PinTable<int, 4> t;
  t.add(5, &a_obj);
  TEST_ASSERT_TRUE(t.pin(5, LOOP) == &a_obj);
  TEST_ASSERT_TRUE(t.pin(5, LOOP) == &a_obj);  // nested pin, same task
  const int slot = t.close(5, ASYNC);
  TEST_ASSERT_TRUE(slot >= 0);
  TEST_ASSERT_TRUE(t.waiting(slot));
  TEST_ASSERT_TRUE(t.pin(5, LOOP) == nullptr);  // closing: no new pins
  TEST_ASSERT_TRUE(t.peek(5) == nullptr);
  uint32_t ids[4];
  TEST_ASSERT_EQUAL(0, (int)t.ids(ids, 4));      // not listed any more
  int s = -1;
  TEST_ASSERT_FALSE(t.unpin(5, LOOP, s));        // one pin left
  TEST_ASSERT_TRUE(t.waiting(slot));
  TEST_ASSERT_TRUE(t.unpin(5, LOOP, s));         // last one: wake the closer
  TEST_ASSERT_EQUAL(slot, s);
  TEST_ASSERT_FALSE(t.waiting(slot));
  t.done(slot);
  TEST_ASSERT_FALSE(t.has(5));
  TEST_ASSERT_FALSE(t.unpin(5, LOOP, s));        // a late unpin is harmless
}

// The client freed inside its own sender's call (same task): no self-wait.
static void test_close_on_own_task(void) {
  PinTable<int, 4> t;
  t.add(3, &a_obj);
  t.pin(3, LOOP);
  TEST_ASSERT_EQUAL(-1, t.close(3, LOOP));
  TEST_ASSERT_FALSE(t.has(3));
  int s = -1;
  TEST_ASSERT_FALSE(t.unpin(3, LOOP, s));
}

// Pins from the closer's task and another task: waits for the other only.
static void test_close_mixed(void) {
  PinTable<int, 4> t;
  t.add(4, &b_obj);
  t.pin(4, ASYNC);
  t.pin(4, LOOP);
  const int slot = t.close(4, ASYNC);
  TEST_ASSERT_TRUE(slot >= 0);
  int s = -1;
  TEST_ASSERT_TRUE(t.unpin(4, LOOP, s));  // the other task's last pin
  TEST_ASSERT_FALSE(t.waiting(slot));
  t.done(slot);
}

// At most kUsers tasks pin one entry at once; a third is told to skip.
static void test_pin_users_cap(void) {
  PinTable<int, 2> t;
  t.add(1, &a_obj);
  TEST_ASSERT_TRUE(t.pin(1, LOOP) != nullptr);
  TEST_ASSERT_TRUE(t.pin(1, ASYNC) != nullptr);
  TEST_ASSERT_TRUE(t.pin(1, OTHER) == nullptr);
  int s = -1;
  t.unpin(1, ASYNC, s);
  TEST_ASSERT_TRUE(t.pin(1, OTHER) != nullptr);
  // A timed-out closer frees the slot; late unpins find nothing.
  const int slot = t.close(1, ASYNC);
  TEST_ASSERT_TRUE(slot >= 0);
  t.done(slot);
  TEST_ASSERT_FALSE(t.unpin(1, LOOP, s));
  TEST_ASSERT_FALSE(t.unpin(1, OTHER, s));
  TEST_ASSERT_TRUE(t.add(2, &b_obj));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ms_until_due);
  RUN_TEST(test_log_this_fail);
  RUN_TEST(test_pin_basic);
  RUN_TEST(test_close_waits_for_other_task);
  RUN_TEST(test_close_on_own_task);
  RUN_TEST(test_close_mixed);
  RUN_TEST(test_pin_users_cap);
  return UNITY_END();
}
