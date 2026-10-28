// Host tests of the flash write gate's rules (power/WriteGateCore) and of
// WriteGate's host build (one thread, a set clock).

#include <unity.h>

#include <power/PowerGuard.h>
#include <power/WriteGate.h>
#include <power/WriteGateCore.h>

#include "WriteGateHost.h"

using namespace ESPRack;
using V = WriteGateCore::Verdict;

static WriteGateCore g;

void setUp(void) {
  g = WriteGateCore{};
  WriteGateConfig c;
  c.idleMs = 1000;  // the tests' timings (the default is 4 s)
  g.configure(c);
  PowerGuard::resetForTest();
  writeGateHostReset();
}
void tearDown(void) {}

static bool go(uint32_t now, uint32_t since, WriteGateCore::Ticket t = 0) {
  return g.enter(now, since, false, t).verdict == V::Go;
}

static void test_free_without_windows(void) {
  // No holder ever opened a window: every writer goes, several at once.
  TEST_ASSERT_TRUE(go(0, 0));
  TEST_ASSERT_TRUE(go(1, 1));
  TEST_ASSERT_TRUE(go(2, 2));
  TEST_ASSERT_EQUAL_UINT8(3, g.writing());
  g.leave(3, 0);
  g.leave(3, 1);
  g.leave(3, 2);
  TEST_ASSERT_EQUAL_UINT8(0, g.writing());
  TEST_ASSERT_TRUE(g.isFree(3));
  TEST_ASSERT_EQUAL_UINT32(3, g.stats().longestFreeHoldMs);  // free: not a write between windows
  TEST_ASSERT_EQUAL_UINT32(0, g.stats().longestHoldMs);
}

static void test_window_then_one_writer_per_pause(void) {
  const int8_t h = g.addHolder("bus");
  TEST_ASSERT_EQUAL_INT8(0, h);
  g.open(h, 100);
  WriteGateCore::Decision d = g.enter(110, 110);
  TEST_ASSERT_TRUE(d.verdict == V::Wait);
  TEST_ASSERT_TRUE(d.retryMs > 0 && d.retryMs <= 2000);

  g.close(h, 150, 90);                // the next window 90 ms from now at the earliest
  TEST_ASSERT_TRUE(go(151, 110));     // one writer
  TEST_ASSERT_FALSE(go(152, 152));    // the next waits for the next pause
  g.leave(160, 151);
  TEST_ASSERT_FALSE(go(160, 152));    // still this pause: used up

  g.open(h, 240);
  g.close(h, 270, 90);
  TEST_ASSERT_TRUE(go(271, 152));     // the next pause takes it
  g.leave(280, 271);
  TEST_ASSERT_EQUAL_UINT32(2, g.stats().inPause);
  TEST_ASSERT_EQUAL_UINT32(0, g.stats().forced);
  TEST_ASSERT_EQUAL_UINT32(2, g.stats().windows);
}

static void test_forecast_left_at_start(void) {
  const int8_t h = g.addHolder("bus");
  g.open(h, 0);
  g.close(h, 30, 90);                 // no window before 120
  TEST_ASSERT_TRUE(g.wouldGo(60, 60));   // 60 ms left: enough
  TEST_ASSERT_FALSE(g.wouldGo(61, 61));  // 59 ms left: a late writer does not start
  TEST_ASSERT_FALSE(go(61, 61));
}

static void test_short_or_no_pause(void) {
  const int8_t h = g.addHolder("bus");
  g.open(h, 0);
  g.close(h, 30, 20);                 // 20 ms: shorter than minPauseMs (60)
  TEST_ASSERT_FALSE(go(31, 31));
  // A writer that has waited maxWaitMs (10 s) takes a short pause anyway...
  g.open(h, 9990);
  g.close(h, 10020, 20);
  TEST_ASSERT_TRUE(go(10021, 0));
  TEST_ASSERT_EQUAL_UINT32(1, g.stats().forced);
  g.leave(10030, 10021);
  // ... but not "no pause" (0) or a forecast already behind us.
  g.open(h, 10100);
  g.close(h, 10120, 0);
  TEST_ASSERT_FALSE(go(10121, 0));
  g.open(h, 10200);
  g.close(h, 10230, 5);
  TEST_ASSERT_FALSE(go(10240, 0));    // 5 ms forecast, 10 ms gone: none left
}

static void test_unknown_pause_takes_a_writer(void) {
  const int8_t h = g.addHolder("bus");
  g.open(h, 0);
  g.close(h, 30);                     // no forecast: any pause
  TEST_ASSERT_TRUE(go(31, 31));
  g.leave(32, 31);
}

static void test_window_expires(void) {
  const int8_t h = g.addHolder("bus");
  g.open(h, 1000);
  g.open(h, 2500);                        // opening an open window doesn't extend it
  TEST_ASSERT_TRUE(g.windowOpen(2999));
  TEST_ASSERT_FALSE(g.windowOpen(3000));  // maxWindowMs (2 s) after the first open
  TEST_ASSERT_EQUAL_UINT32(1, g.stats().expired);
  TEST_ASSERT_EQUAL_UINT32(1, g.stats().windows);
  TEST_ASSERT_TRUE(go(3001, 3001));       // its end is a pause without a forecast
  g.leave(3002, 3001);
  g.close(h, 3100, 90);                   // a late close of the expired window: nothing
  TEST_ASSERT_EQUAL_UINT32(1, g.stats().expired);
  TEST_ASSERT_TRUE(g.isFree(4000));
}

static void test_back_to_back_windows_force_a_writer(void) {
  const int8_t h = g.addHolder("bus");
  uint32_t t = 0;
  // The holder reopens at once every time: no usable pause.
  for (; t < 12000; t += 100) {
    g.open(h, t);
    TEST_ASSERT_FALSE(go(t + 1, 0));
    g.close(h, t + 90, 5);
  }
  g.open(h, t);
  // Waited past maxWaitMs + maxWindowMs (12 s): it goes inside the window.
  TEST_ASSERT_TRUE(go(t + 1, 0));
  TEST_ASSERT_EQUAL_UINT32(1, g.stats().forced);
  TEST_ASSERT_EQUAL_UINT32(1, g.stats().overlaps);  // a write inside a window
  g.leave(t + 2, t + 1);
}

static void test_idle_frees_the_gate(void) {
  const int8_t h = g.addHolder("bus");
  g.open(h, 0);
  g.close(h, 50);                         // no forecast
  TEST_ASSERT_TRUE(go(51, 51));
  TEST_ASSERT_FALSE(go(52, 52));
  g.leave(60, 51);
  TEST_ASSERT_FALSE(g.isFree(1049));
  TEST_ASSERT_TRUE(g.isFree(1050));       // idleMs (1 s) without a window
  TEST_ASSERT_TRUE(go(1050, 52));         // free
  TEST_ASSERT_FALSE(go(1051, 1051));      // one at a time: windows were seen
  g.leave(1060, 1050);
  TEST_ASSERT_TRUE(go(1061, 1051));
  g.leave(1070, 1061);
}

static void test_forecast_limits_the_free_gate(void) {
  const int8_t h = g.addHolder("bus");
  g.open(h, 0);
  g.close(h, 0, 1030);                    // a slow holder: the next window at 1030
  // Idle by the clock at 1000, but a window is 30 ms away: no writer.
  TEST_ASSERT_FALSE(g.isFree(1000));
  TEST_ASSERT_TRUE(g.enter(1000, 1000).verdict == V::Wait);
  TEST_ASSERT_TRUE(g.enter(1000, 1000, true).verdict == V::Wait);
  // Overdue: it still counts (a window may open any moment) until idleMs past it.
  TEST_ASSERT_TRUE(g.enter(1031, 1031).verdict == V::Wait);
  TEST_ASSERT_TRUE(g.enter(2030, 2030).verdict == V::Go);
  g.leave(2031, 2030);
  TEST_ASSERT_TRUE(g.isFree(2031));
  // A forecast far enough off leaves the gate free, for big writes too.
  const int8_t s = g.addHolder("slow");
  g.open(s, 3000);
  g.close(s, 3010, 5000);
  TEST_ASSERT_TRUE(g.isFree(4010));
  TEST_ASSERT_TRUE(g.enter(4010, 4010, true).verdict == V::Go);
  g.leave(4011, 4010);
}

static void test_near_forecast_is_a_pause_not_free(void) {
  const int8_t h = g.addHolder("bus");
  g.open(h, 0);
  g.close(h, 0, 1200);                    // idle at 1000, the next window at 1200
  const WriteGateCore::Ticket a = g.join(1000);
  const WriteGateCore::Ticket b = g.join(1000);
  TEST_ASSERT_TRUE(go(1000, 1000, a));    // 200 ms left: one writer...
  g.quit(a);
  TEST_ASSERT_FALSE(go(1000, 1000, b));   // ... not all of them at once
  TEST_ASSERT_FALSE(go(1001, 1001));
  g.leave(1050, 1000);
  TEST_ASSERT_FALSE(go(1051, 1000, b));   // and one per pause
  g.quit(b);
}

static void test_one_at_a_time_once_windows_were_seen(void) {
  const int8_t h = g.addHolder("bus");
  g.open(h, 0);
  g.close(h, 10);                         // free from 1010
  TEST_ASSERT_TRUE(go(1010, 1010));       // the first write while free...
  WriteGateCore::Decision d = g.enter(1011, 1011);
  TEST_ASSERT_TRUE(d.verdict == V::Wait); // ... a second waits for it, not polling hard
  TEST_ASSERT_TRUE(d.retryMs >= 1000);
  g.leave(1020, 1010);
  TEST_ASSERT_TRUE(go(1021, 1011));
  g.leave(1030, 1021);
}

static void test_clock_never_goes_back(void) {
  const int8_t h = g.addHolder("bus");
  g.open(h, 1002);                        // a holder on another core opened at 1002...
  // ... and a writer that read 1000 before the lock: no "expired" window.
  TEST_ASSERT_TRUE(g.enter(1000, 1000).verdict == V::Wait);
  TEST_ASSERT_EQUAL_UINT32(0, g.stats().expired);
  TEST_ASSERT_TRUE(g.windowOpen(1001));
}

static void test_several_holders(void) {
  const int8_t a = g.addHolder("a");
  const int8_t b = g.addHolder("b");
  g.open(a, 0);
  g.open(b, 10);
  g.close(a, 40, 90);                     // a: none before 130
  TEST_ASSERT_FALSE(go(41, 41));          // b still needs the CPU
  TEST_ASSERT_EQUAL_UINT8(1, g.openWindows(41));
  g.close(b, 60, 300);                    // b: none before 360
  TEST_ASSERT_TRUE(g.wouldGo(61, 41));    // 69 ms left before a's
  TEST_ASSERT_FALSE(g.wouldGo(80, 41));   // 50 ms: the earliest forecast decides
  g.open(a, 130);
  g.close(a, 160);                        // a: no forecast now; b's still counts
  TEST_ASSERT_TRUE(go(161, 41));          // 199 ms left before b's
  g.leave(170, 161);
  for (int i = 2; i < WriteGateCore::kMaxHolders; ++i) TEST_ASSERT_TRUE(g.addHolder("x") >= 0);
  TEST_ASSERT_EQUAL_INT8(-1, g.addHolder("one too many"));
  TEST_ASSERT_EQUAL_STRING("b", g.holderName(b));
}

static void test_stale_forecast_stops_counting(void) {
  const int8_t a = g.addHolder("a");
  const int8_t b = g.addHolder("b");
  g.open(a, 0);
  g.close(a, 10, 50);                     // a: none before 60; then a falls silent
  for (uint32_t t = 100; t < 2200; t += 100) {
    g.open(b, t);
    g.close(b, t + 20);                   // b keeps the bus busy, no forecast
  }
  // At 2121 a's forecast is over 2 s behind: it no longer holds the writer.
  TEST_ASSERT_TRUE(go(2121, 2121));
  g.leave(2122, 2121);
}

static void test_queue_first_come_first_served(void) {
  const int8_t h = g.addHolder("bus");
  g.open(h, 0);
  const WriteGateCore::Ticket t1 = g.join(1);
  const WriteGateCore::Ticket t2 = g.join(2);
  TEST_ASSERT_EQUAL_UINT8(2, g.queued());
  g.close(h, 30, 90);
  TEST_ASSERT_FALSE(go(31, 2, t2));       // the second may not jump the first
  TEST_ASSERT_FALSE(go(31, 31));          // nor may a writer that doesn't wait
  TEST_ASSERT_TRUE(go(31, 1, t1));
  g.quit(t1);
  g.leave(40, 31);
  g.open(h, 120);
  // A window that opened meanwhile sends the next one back: its place kept.
  TEST_ASSERT_FALSE(go(121, 2, t2));
  g.close(h, 150, 90);
  TEST_ASSERT_TRUE(go(151, 2, t2));
  g.quit(t2);
  g.leave(160, 151);
  TEST_ASSERT_EQUAL_UINT8(0, g.queued());
}

static void test_overlaps_are_counted(void) {
  const int8_t h = g.addHolder("bus");
  g.open(h, 0);
  g.close(h, 30, 90);
  TEST_ASSERT_TRUE(go(31, 31));           // a write starts in the pause...
  g.open(h, 100);                         // ... and is still under way at the next window
  g.leave(140, 31);                       // 40 ms together
  TEST_ASSERT_EQUAL_UINT32(1, g.stats().overlaps);
  TEST_ASSERT_EQUAL_UINT32(40, g.stats().longestOverlapMs);
  TEST_ASSERT_EQUAL_UINT32(109, g.stats().longestHoldMs);
  g.close(h, 200, 90);
  TEST_ASSERT_TRUE(go(201, 201));
  g.leave(210, 201);                      // inside its pause: no overlap
  TEST_ASSERT_EQUAL_UINT32(1, g.stats().overlaps);
}

static void test_one_write_at_a_time_between_windows(void) {
  const int8_t h = g.addHolder("bus");
  g.open(h, 0);
  g.close(h, 30, 90);
  TEST_ASSERT_TRUE(go(31, 31));           // A, still writing
  g.open(h, 120);
  g.close(h, 150, 90);
  TEST_ASSERT_FALSE(go(151, 31));         // B: A has not finished
  g.leave(152, 31);
  TEST_ASSERT_TRUE(go(152, 31));          // the same pause, A done
  g.leave(160, 152);
}

static void test_power_decides_first(void) {
  PowerGuard::setSupply(SupplyState::PowerFail);
  TEST_ASSERT_TRUE(g.enter(0, 0).verdict == V::Refused);
  TEST_ASSERT_TRUE(g.force(0, 0).verdict == V::Refused);
  TEST_ASSERT_EQUAL_UINT8(0, g.writing());
  PowerGuard::setSupply(SupplyState::AcOk);
  TEST_ASSERT_TRUE(go(1, 1));
  g.leave(2, 1);
  PowerGuard::latchWritesOffUntilRestart();
  TEST_ASSERT_TRUE(g.enter(2, 2).verdict == V::Refused);
  TEST_ASSERT_EQUAL_UINT32(3, g.stats().refused);
}

static void test_only_free_and_would_go(void) {
  const int8_t h = g.addHolder("bus");
  g.open(h, 0);
  g.close(h, 30, 90);
  TEST_ASSERT_FALSE(g.wouldGo(31, 31, true));   // a pause is not free
  TEST_ASSERT_TRUE(g.wouldGo(31, 31));           // a plain writer would go
  TEST_ASSERT_TRUE(g.wouldGo(32, 32));           // and asking took nothing
  TEST_ASSERT_EQUAL_UINT32(0, g.stats().grants);
  WriteGateCore::Decision d = g.enter(31, 31, true);
  TEST_ASSERT_TRUE(d.verdict == V::Wait);
  TEST_ASSERT_EQUAL_UINT32(998, d.retryMs);      // until idle (the clock is at 32: it never goes back)
  d = g.enter(1030, 31, true);                   // idle, but the forecast (120) still counts
  TEST_ASSERT_TRUE(d.verdict == V::Wait);
  TEST_ASSERT_EQUAL_UINT32(91, d.retryMs);       // until it stops: idleMs past it
  TEST_ASSERT_TRUE(g.enter(1120, 31, true).verdict == V::Go);
  g.leave(1121, 1120);
}

static void test_force_uses_the_pause(void) {
  const int8_t h = g.addHolder("bus");
  g.open(h, 0);
  TEST_ASSERT_TRUE(g.force(10, 0).verdict == V::Go);  // a bound ran out: inside the window
  TEST_ASSERT_EQUAL_UINT32(1, g.stats().overlaps);
  g.close(h, 30, 90);                                 // the overlap ends with the window
  TEST_ASSERT_EQUAL_UINT32(20, g.stats().longestOverlapMs);
  g.leave(31, 10);
  TEST_ASSERT_EQUAL_UINT32(21, g.stats().longestHoldMs);  // a forced write counts as one between windows
  TEST_ASSERT_TRUE(go(31, 31));                       // the pause is still free for one
  g.leave(32, 31);
  TEST_ASSERT_EQUAL_UINT32(1, g.stats().forced);
}

static void test_clock_wraps(void) {
  const uint32_t t0 = 0xFFFFFF00u;
  const int8_t h = g.addHolder("bus");
  g.open(h, t0);
  TEST_ASSERT_TRUE(g.windowOpen(t0 + 1000));        // wrapped past 0
  g.close(h, t0 + 1100, 90);
  TEST_ASSERT_TRUE(go(t0 + 1101, t0 + 1101));
  g.leave(t0 + 1110, t0 + 1101);
  TEST_ASSERT_FALSE(g.isFree(t0 + 2100));            // its forecast (t0+1190) still counts
  TEST_ASSERT_TRUE(g.isFree(t0 + 2190));
}

static void test_host_write_gate(void) {
  // The WriteGate API over the same rules (the host build: one thread).
  TEST_ASSERT_TRUE(WriteGate::tryEnter());          // no holders: free
  WriteGate::leave(0);
  const int8_t h = WriteGate::addHolder("bus");
  writeGateHostSetNow(100);
  WriteGate::open(h);
  TEST_ASSERT_TRUE(WriteGate::inWindow());
  {
    WritePass p;
    TEST_ASSERT_FALSE(p.ok());
  }
  writeGateHostSetNow(130);
  WriteGate::close(h, 90);
  {
    WritePass p(WriteGate::now(), true);
    TEST_ASSERT_FALSE(p.ok());                      // only while free: not in a pause
  }
  {
    WritePass p;
    TEST_ASSERT_TRUE(p.ok());
    TEST_ASSERT_EQUAL_UINT8(1, WriteGate::writing());
  }
  TEST_ASSERT_EQUAL_UINT8(0, WriteGate::writing());
  TEST_ASSERT_EQUAL_UINT32(1, WriteGate::stats().inPause);
  TEST_ASSERT_EQUAL_STRING("bus", WriteGate::holderName(h));
  WriteGate::resetStats();
  TEST_ASSERT_EQUAL_UINT32(0, WriteGate::stats().grants);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_free_without_windows);
  RUN_TEST(test_window_then_one_writer_per_pause);
  RUN_TEST(test_forecast_left_at_start);
  RUN_TEST(test_short_or_no_pause);
  RUN_TEST(test_unknown_pause_takes_a_writer);
  RUN_TEST(test_window_expires);
  RUN_TEST(test_back_to_back_windows_force_a_writer);
  RUN_TEST(test_idle_frees_the_gate);
  RUN_TEST(test_forecast_limits_the_free_gate);
  RUN_TEST(test_near_forecast_is_a_pause_not_free);
  RUN_TEST(test_one_at_a_time_once_windows_were_seen);
  RUN_TEST(test_clock_never_goes_back);
  RUN_TEST(test_several_holders);
  RUN_TEST(test_stale_forecast_stops_counting);
  RUN_TEST(test_queue_first_come_first_served);
  RUN_TEST(test_overlaps_are_counted);
  RUN_TEST(test_one_write_at_a_time_between_windows);
  RUN_TEST(test_power_decides_first);
  RUN_TEST(test_only_free_and_would_go);
  RUN_TEST(test_force_uses_the_pause);
  RUN_TEST(test_clock_wraps);
  RUN_TEST(test_host_write_gate);
  return UNITY_END();
}
