// SES-LIB-006 acceptance 6: LimitMonitor tables — hysteresis, trip / clear
// dwell, invalid samples restart the dwell and never change the state.
#include <math.h>
#include <unity.h>

#include <ESPRackPure/LimitMonitor.h>

using namespace ESPRack;
using Ev = LimitMonitor::Ev;

void setUp(void) {}
void tearDown(void) {}

struct Row {
  float v;
  bool valid;
  uint64_t mono;
  Ev ev;
  bool tripped;
};

static void run(LimitMonitor& m, const Row* rows, int n) {
  for (int i = 0; i < n; ++i) {
    const Ev ev = m.feed(rows[i].v, rows[i].valid, rows[i].mono);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)rows[i].ev, (int)ev, "event");
    TEST_ASSERT_EQUAL_MESSAGE(rows[i].tripped, m.tripped(), "tripped");
  }
}

static const float NaN = NAN;

static void test_upper_hysteresis_and_dwell(void) {
  LimitMonitor m({10.0f, 1.0f, true, 1000, 2000});
  const Row rows[] = {
      {9.0f, true, 0, Ev::None, false},
      {10.0f, true, 100, Ev::None, false},     // == limit does not exceed
      {10.5f, true, 200, Ev::None, false},     // pending since 200
      {11.0f, true, 1100, Ev::None, false},    // 900 ms
      {11.0f, true, 1200, Ev::Tripped, true},  // 1000 ms
      {9.5f, true, 1300, Ev::None, true},      // inside the band: no clear pending
      {8.9f, true, 1400, Ev::None, true},      // clear pending since 1400
      {8.0f, false, 2000, Ev::None, true},     // invalid: restart, state kept
      {8.5f, true, 2100, Ev::None, true},      // pending since 2100
      {8.5f, true, 4099, Ev::None, true},
      {8.5f, true, 4100, Ev::Cleared, false},
      {11.0f, true, 5000, Ev::None, false},    // pending since 5000
      {NaN, true, 5500, Ev::None, false},      // NaN counts as invalid
      {11.0f, true, 6000, Ev::None, false},    // pending since 6000
      {11.0f, true, 6999, Ev::None, false},
      {11.0f, true, 7000, Ev::Tripped, true},
  };
  run(m, rows, (int)(sizeof rows / sizeof rows[0]));
}

static void test_interrupted_dwell_restarts(void) {
  LimitMonitor m({10.0f, 1.0f, true, 1000, 0});
  const Row rows[] = {
      {11.0f, true, 0, Ev::None, false},
      {10.0f, true, 500, Ev::None, false},     // back under: restart
      {11.0f, true, 600, Ev::None, false},
      {11.0f, true, 1599, Ev::None, false},
      {11.0f, true, 1600, Ev::Tripped, true},
      {9.0f, true, 1700, Ev::None, true},      // not below limit - hyst (9.0)
      {8.99f, true, 1800, Ev::Cleared, false}, // clearMs 0: immediate
  };
  run(m, rows, (int)(sizeof rows / sizeof rows[0]));
}

static void test_lower_limit(void) {
  LimitMonitor m({5.0f, 0.5f, false, 0, 0});
  const Row rows[] = {
      {5.0f, true, 0, Ev::None, false},
      {4.9f, true, 1, Ev::Tripped, true},
      {5.4f, true, 2, Ev::None, true},   // not above limit + hyst
      {5.5f, true, 3, Ev::None, true},
      {5.6f, true, 4, Ev::Cleared, false},
      {4.0f, false, 5, Ev::None, false},  // invalid never trips
  };
  run(m, rows, (int)(sizeof rows / sizeof rows[0]));
}

static void test_mono_backwards_and_reset(void) {
  LimitMonitor m({10.0f, 1.0f, true, 1000, 1000});
  TEST_ASSERT_EQUAL_INT((int)Ev::None, (int)m.feed(11.0f, true, 10000));
  TEST_ASSERT_EQUAL_INT((int)Ev::None, (int)m.feed(11.0f, true, 9000));  // clock went back: no dwell
  TEST_ASSERT_EQUAL_INT((int)Ev::Tripped, (int)m.feed(11.0f, true, 11000));
  m.reset();
  TEST_ASSERT_FALSE(m.tripped());
  TEST_ASSERT_EQUAL_INT((int)Ev::None, (int)m.feed(11.0f, true, 12000));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_upper_hysteresis_and_dwell);
  RUN_TEST(test_interrupted_dwell_restarts);
  RUN_TEST(test_lower_limit);
  RUN_TEST(test_mono_backwards_and_reset);
  return UNITY_END();
}
