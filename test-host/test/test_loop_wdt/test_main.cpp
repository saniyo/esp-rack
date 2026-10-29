// LoopWdt::Settle: pause/resume never skipped, the watchdog ends in line
// with armed/paused when transitions on two tasks overlap.
#include <unity.h>

#include <system/LoopWdt.h>

using ESPRack::LoopWdt::Settle;

void setUp(void) {}
void tearDown(void) {}

// One task's whole settle(): begin, the step, end, again while asked.
static int settleAll(Settle& s, int& enables, int& disables) {
  int steps = 0;
  for (;;) {
    const int step = s.begin();
    if (step == 0) return steps;
    ++steps;
    if (step > 0) ++enables;
    else ++disables;
    if (!s.end()) return steps;
  }
}

static void test_unarmed_does_nothing(void) {
  Settle s;
  int en = 0, dis = 0;
  s.paused++;
  TEST_ASSERT_EQUAL(0, settleAll(s, en, dis));
  s.paused--;
  TEST_ASSERT_EQUAL(0, settleAll(s, en, dis));
  TEST_ASSERT_FALSE(s.on);
}

static void test_arm_pause_resume(void) {
  Settle s;
  int en = 0, dis = 0;
  s.armed = true;
  settleAll(s, en, dis);
  TEST_ASSERT_TRUE(s.on);
  TEST_ASSERT_EQUAL(1, en);
  s.paused++;  // nested pauses: one disable
  settleAll(s, en, dis);
  s.paused++;
  settleAll(s, en, dis);
  TEST_ASSERT_FALSE(s.on);
  TEST_ASSERT_EQUAL(1, dis);
  s.paused--;
  settleAll(s, en, dis);
  TEST_ASSERT_FALSE(s.on);  // still paused once
  s.paused--;
  settleAll(s, en, dis);
  TEST_ASSERT_TRUE(s.on);
  TEST_ASSERT_EQUAL(2, en);
}

static void test_armed_while_paused(void) {
  Settle s;
  int en = 0, dis = 0;
  s.paused = 1;
  s.armed = true;
  settleAll(s, en, dis);
  TEST_ASSERT_FALSE(s.on);
  TEST_ASSERT_EQUAL(0, en + dis);
  s.paused = 0;
  settleAll(s, en, dis);
  TEST_ASSERT_TRUE(s.on);
}

// Task A resumes (enable under way); task B pauses meanwhile and finds A
// busy: B returns at once, A sees the change and disables.
static void test_overlap_pause_during_enable(void) {
  Settle s;
  s.armed = true;
  s.paused = 1;
  s.on = false;
  s.paused--;                       // A: resume
  TEST_ASSERT_EQUAL(1, s.begin());  // A: enabling, no lock held
  s.paused++;                       // B: pause
  TEST_ASSERT_EQUAL(0, s.begin());  // B: A is on it
  TEST_ASSERT_TRUE(s.end());        // A: done, look again
  TEST_ASSERT_EQUAL(-1, s.begin()); // A: disable for B
  TEST_ASSERT_FALSE(s.end());
  TEST_ASSERT_FALSE(s.on);
  TEST_ASSERT_EQUAL(0, s.begin());  // in line
}

// Overlap that cancels out: pause+resume while an enable runs ends enabled.
static void test_overlap_cancels(void) {
  Settle s;
  s.armed = true;
  TEST_ASSERT_EQUAL(1, s.begin());
  s.paused++;
  TEST_ASSERT_EQUAL(0, s.begin());
  s.paused--;
  TEST_ASSERT_EQUAL(0, s.begin());
  TEST_ASSERT_TRUE(s.end());
  TEST_ASSERT_EQUAL(0, s.begin());  // wanted on, is on
  TEST_ASSERT_TRUE(s.on);
  TEST_ASSERT_FALSE(s.busy);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_unarmed_does_nothing);
  RUN_TEST(test_arm_pause_resume);
  RUN_TEST(test_armed_while_paused);
  RUN_TEST(test_overlap_pause_during_enable);
  RUN_TEST(test_overlap_cancels);
  return UNITY_END();
}
