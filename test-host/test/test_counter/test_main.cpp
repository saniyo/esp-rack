// SES-LIB-006 acceptance 6: CounterTracker test vectors V1–V18 (verbatim from
// the design, negSlack 3, maxHold 5, modulo 0, maxA = maxH = 10, epoch 1,
// valid unless stated) plus state checks.
#include <unity.h>

#include <ESPRackPure/CounterTracker.h>

using namespace ESPRack;
using Ev = CounterEvent;

void setUp(void) {}
void tearDown(void) {}

static CounterTracker tracker(uint64_t anchor, bool valid = true, uint64_t modulo = 0) {
  CounterCfg c;
  c.negSlack = 3;
  c.maxHold = 5;
  c.modulo = modulo;
  CounterTracker t(c);
  t.restore(anchor, 1, valid);
  return t;
}
static CounterStep feed(CounterTracker& t, uint64_t v, uint16_t epoch = 1, bool evidence = false) {
  return t.feed(v, epoch, 10, 10, evidence);
}
#define EXPECT(STEP, CREDIT, EVENT)                                                       \
  do {                                                                                     \
    const CounterStep s_ = (STEP);                                                         \
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)Ev::EVENT, (int)s_.ev, "event, expected " #EVENT); \
    TEST_ASSERT_EQUAL_INT64((CREDIT), s_.credit);                                          \
  } while (0)

static void test_v1_to_v4(void) {
  CounterTracker t = tracker(0, false);  // V1
  EXPECT(feed(t, 1000), 0, First);
  TEST_ASSERT_EQUAL_UINT64(1000, t.anchor());
  TEST_ASSERT_TRUE(t.valid());

  t = tracker(1000);  // V2
  const CounterStep s = feed(t, 1004);
  EXPECT(s, 4, Normal);
  TEST_ASSERT_EQUAL_UINT64(1000, s.before);
  TEST_ASSERT_EQUAL_UINT64(1004, s.after);
  TEST_ASSERT_EQUAL_UINT64(1004, t.anchor());

  t = tracker(1000);  // V3
  EXPECT(feed(t, 1000), 0, Normal);
  TEST_ASSERT_EQUAL_UINT64(1000, t.anchor());

  t = tracker(1000);  // V4
  EXPECT(feed(t, 998, 1, true), 0, NegSlack);
  TEST_ASSERT_EQUAL_UINT64(998, t.anchor());
}

static void test_v5_to_v10_held_then_resolved(void) {
  CounterTracker t = tracker(1000);  // V5: small negative without evidence holds
  const CounterStep h = feed(t, 998);
  EXPECT(h, 0, Held);
  TEST_ASSERT_EQUAL_UINT64(1000, h.before);
  TEST_ASSERT_EQUAL_UINT64(1000, h.after);
  TEST_ASSERT_TRUE(t.holding());
  TEST_ASSERT_EQUAL_UINT8(1, t.heldCount());
  EXPECT(feed(t, 1001), 1, GlitchDropped);
  TEST_ASSERT_EQUAL_UINT64(1001, t.anchor());
  TEST_ASSERT_FALSE(t.holding());
  TEST_ASSERT_EQUAL_UINT8(0, t.heldCount());

  t = tracker(1000);  // V6
  EXPECT(feed(t, 5000), 0, Held);
  EXPECT(feed(t, 1003), 3, GlitchDropped);
  TEST_ASSERT_EQUAL_UINT64(1003, t.anchor());

  t = tracker(1000);  // V7: reset, the held value itself is credited (h <= heldMaxA)
  EXPECT(feed(t, 2), 0, Held);
  EXPECT(feed(t, 5), 5, ResetConfirmed);
  TEST_ASSERT_EQUAL_UINT64(5, t.anchor());

  t = tracker(1000);  // V8
  EXPECT(feed(t, 50), 0, Held);
  EXPECT(feed(t, 53), 3, ResetConfirmed);
  TEST_ASSERT_EQUAL_UINT64(53, t.anchor());

  t = tracker(1000);  // V9: a jump is never credited, only what follows it
  EXPECT(feed(t, 900000), 0, Held);
  EXPECT(feed(t, 900004), 4, JumpConfirmed);
  TEST_ASSERT_EQUAL_UINT64(900004, t.anchor());

  t = tracker(1000);  // V10
  EXPECT(feed(t, 400), 0, Held);
  EXPECT(feed(t, 402), 2, ResetConfirmed);
  TEST_ASSERT_EQUAL_UINT64(402, t.anchor());
}

static void test_v11_forced_rebase(void) {
  CounterTracker t = tracker(1000);
  const uint64_t seq[4] = {5000, 9000, 13000, 17000};
  for (int i = 0; i < 4; ++i) {
    EXPECT(feed(t, seq[i]), 0, Held);
    TEST_ASSERT_EQUAL_UINT8(i + 1, t.heldCount());
    TEST_ASSERT_EQUAL(i + 1 >= 3, t.degraded());  // degraded from the 3rd
    TEST_ASSERT_EQUAL_UINT64(1000, t.anchor());
  }
  EXPECT(feed(t, 21000), 0, ForcedRebase);
  TEST_ASSERT_EQUAL_UINT64(21000, t.anchor());
  TEST_ASSERT_FALSE(t.holding());
  TEST_ASSERT_FALSE(t.degraded());
}

static void test_v12_v13_wrap(void) {
  CounterTracker t = tracker(9999995, true, 10000000);  // V12
  EXPECT(feed(t, 3), 8, Wrap);
  TEST_ASSERT_EQUAL_UINT64(3, t.anchor());

  t = tracker(9999980, true, 10000000);  // V13: too far from the top to be a wrap
  EXPECT(feed(t, 3), 0, Held);
  TEST_ASSERT_EQUAL_UINT64(9999980, t.anchor());

  t = tracker(9999990, true, 10000000);  // boundary: a = modulo - maxA, credit = maxA
  EXPECT(feed(t, 0), 10, Wrap);
}

static void test_v14_to_v16_expected_reset(void) {
  CounterTracker t = tracker(1000);  // V14
  EXPECT(feed(t, 2, 2), 2, ExpectedReset);
  TEST_ASSERT_EQUAL_UINT64(2, t.anchor());
  TEST_ASSERT_EQUAL_UINT16(2, t.epoch());

  t = tracker(1000);  // V15
  EXPECT(feed(t, 50, 2), 0, ExpectedReset);
  TEST_ASSERT_EQUAL_UINT64(50, t.anchor());

  t = tracker(1000);  // V16: an expected reset also drops a held sample
  EXPECT(feed(t, 5000), 0, Held);
  EXPECT(feed(t, 1, 2), 1, ExpectedReset);
  TEST_ASSERT_EQUAL_UINT64(1, t.anchor());
  TEST_ASSERT_FALSE(t.holding());
}

static void test_v17_v18(void) {
  CounterTracker t = tracker(1000);  // V17: beyond the slack even with evidence
  EXPECT(feed(t, 990, 1, true), 0, Held);
  EXPECT(feed(t, 991), 1, ResetConfirmed);
  TEST_ASSERT_EQUAL_UINT64(991, t.anchor());

  t = tracker(1000);  // V18: under-count 11 (A2), only the confirmed step credits
  EXPECT(feed(t, 1011), 0, Held);
  EXPECT(feed(t, 1012), 1, JumpConfirmed);
  TEST_ASSERT_EQUAL_UINT64(1012, t.anchor());
}

static void test_held_max_a_is_from_the_held_sample(void) {
  CounterTracker t = tracker(1000);
  EXPECT(t.feed(2, 1, 1, 10, false), 0, Held);  // held with maxA = 1
  EXPECT(t.feed(5, 1, 10, 10, false), 3, ResetConfirmed);  // h = 2 > heldMaxA -> not credited
  t = tracker(1000);
  EXPECT(t.feed(2, 1, 10, 10, false), 0, Held);
  EXPECT(t.feed(5, 1, 1, 10, false), 5, ResetConfirmed);  // the current maxA does not matter
  // maxH bounds the step from the held value
  t = tracker(1000);
  EXPECT(t.feed(2, 1, 10, 10, false), 0, Held);
  EXPECT(t.feed(20, 1, 10, 10, false), 0, Held);  // 18 > maxH
  TEST_ASSERT_EQUAL_UINT8(2, t.heldCount());
  EXPECT(t.feed(25, 1, 10, 10, false), 5, ResetConfirmed);  // h = 20 > heldMaxA
}

static void test_restore(void) {
  CounterTracker t = tracker(1000);
  feed(t, 5000);
  TEST_ASSERT_TRUE(t.holding());
  t.restore(42, 7, true);
  TEST_ASSERT_FALSE(t.holding());
  TEST_ASSERT_EQUAL_UINT64(42, t.anchor());
  TEST_ASSERT_EQUAL_UINT16(7, t.epoch());
  EXPECT(feed(t, 45, 7), 3, Normal);
  t.restore(42, 7, false);
  EXPECT(feed(t, 45, 9), 0, First);
  TEST_ASSERT_EQUAL_UINT16(9, t.epoch());
  EXPECT(feed(t, 46, 9), 1, Normal);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_v1_to_v4);
  RUN_TEST(test_v5_to_v10_held_then_resolved);
  RUN_TEST(test_v11_forced_rebase);
  RUN_TEST(test_v12_v13_wrap);
  RUN_TEST(test_v14_to_v16_expected_reset);
  RUN_TEST(test_v17_v18);
  RUN_TEST(test_held_max_a_is_from_the_held_sample);
  RUN_TEST(test_restore);
  return UNITY_END();
}
