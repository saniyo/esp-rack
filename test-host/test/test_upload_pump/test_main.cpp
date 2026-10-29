// Host tests of UploadPump's bookkeeping (net/UploadPumpCore): the ring,
// the steps it hands the sink, the TCP window it holds, the order of the
// sink calls and what a failure or a client that leaves does.

#include <unity.h>

#include <net/UploadPumpCore.h>
#include <system/FlashSteps.h>

#include <deque>
#include <vector>

using namespace ESPRack;
using A = UploadPumpCore::Action;
using W = UploadPumpCore::Window;
using Span = UploadPumpCore::Span;

static UploadPumpCore c;
static std::vector<uint8_t> ring;

void setUp(void) { c = UploadPumpCore{}; }
void tearDown(void) {}

static void useRing(uint32_t size) {
  ring.assign(size, 0);
  c.setRing(ring.data(), size);
}

static bool push(uint32_t len, uint8_t fill = 0xA5) {
  Span s[2];
  if (!c.reserve(len, s)) return false;
  for (uint32_t i = 0; i < s[0].len; ++i) s[0].data[i] = fill;
  for (uint32_t i = 0; i < s[1].len; ++i) s[1].data[i] = fill;
  c.commit(len);
  return true;
}

static uint32_t drainStep() {
  Span s[2];
  uint32_t len = 0;
  const A a = c.next(s, len);
  TEST_ASSERT_TRUE(a == A::Write);
  TEST_ASSERT_EQUAL_UINT32(len, s[0].len + s[1].len);
  c.wrote(len, true, true);
  c.done();
  return len;
}

static void test_flash_steps(void) {
  TEST_ASSERT_EQUAL_UINT32(4096, FlashSteps::room(0));
  TEST_ASSERT_EQUAL_UINT32(4095, FlashSteps::room(1));
  TEST_ASSERT_EQUAL_UINT32(4096, FlashSteps::room(8192));
  TEST_ASSERT_EQUAL_UINT32(4096, FlashSteps::step(0, 10000));
  TEST_ASSERT_EQUAL_UINT32(3812, FlashSteps::step(4380, 10000));
  TEST_ASSERT_EQUAL_UINT32(100, FlashSteps::step(4096, 100));
  TEST_ASSERT_EQUAL_UINT32(0, FlashSteps::step(123, 0));
  TEST_ASSERT_EQUAL_UINT32(1, FlashSteps::erases(0, 4096));
  TEST_ASSERT_EQUAL_UINT32(1, FlashSteps::erases(0, 1));
  TEST_ASSERT_EQUAL_UINT32(0, FlashSteps::erases(1, 4095));
  TEST_ASSERT_EQUAL_UINT32(1, FlashSteps::erases(1, 4096));
  TEST_ASSERT_EQUAL_UINT32(1, FlashSteps::erases(4095, 2));
  TEST_ASSERT_EQUAL_UINT32(2, FlashSteps::erases(0, 8192));
  TEST_ASSERT_EQUAL_UINT32(16, FlashSteps::erases(0, 65536));
  TEST_ASSERT_EQUAL_UINT32(0, FlashSteps::erases(5, 0));
  TEST_ASSERT_EQUAL_UINT32(1, FlashSteps::erases(0xFFFFF000u, 4096));  // no wrap at the top
}

static void test_session_order(void) {
  TEST_ASSERT_TRUE(c.begin(1000, 0));
  useRing(16384);
  Span s[2];
  uint32_t len;
  TEST_ASSERT_TRUE(c.next(s, len) == A::Open);
  TEST_ASSERT_TRUE(c.next(s, len) == A::None);  // busy
  c.opened(true);
  c.done();
  TEST_ASSERT_TRUE(c.next(s, len) == A::None);  // nothing yet
  TEST_ASSERT_TRUE(push(5000));
  TEST_ASSERT_EQUAL_UINT32(4096, drainStep());
  TEST_ASSERT_FALSE(c.pending());  // 904: less than a step, more may come
  TEST_ASSERT_TRUE(push(100));
  TEST_ASSERT_FALSE(c.pending());
  c.final();
  TEST_ASSERT_FALSE(c.accepting());
  TEST_ASSERT_FALSE(push(1));  // after final: not kept
  TEST_ASSERT_EQUAL_UINT32(1004, drainStep());  // all in: the rest
  TEST_ASSERT_TRUE(c.next(s, len) == A::Finish);
  c.finished(UploadSink::kMore);
  c.done();
  TEST_ASSERT_TRUE(c.next(s, len) == A::Finish);
  c.finished(UploadSink::kDone);
  c.done();
  TEST_ASSERT_TRUE(c.settled());
  TEST_ASSERT_TRUE(c.result() == UploadResult::Ok);
  TEST_ASSERT_EQUAL_UINT32(5100, c.written());
  TEST_ASSERT_TRUE(c.next(s, len) == A::None);
  TEST_ASSERT_TRUE(c.takeRing() == ring.data());
}

static void test_steps_realign_after_direct_writes(void) {
  TEST_ASSERT_TRUE(c.begin(1000, 0));
  // Gate free: three chunks straight through.
  TEST_ASSERT_TRUE(c.claim());
  c.opened(true);
  for (int i = 0; i < 3; ++i) c.wrote(1460, true, false);
  c.done();
  TEST_ASSERT_EQUAL_UINT32(4380, c.written());
  // Then the ring: the first step ends on the next 4 KB boundary.
  useRing(32768);
  TEST_ASSERT_TRUE(push(20000));
  TEST_ASSERT_FALSE(c.claim());  // data waits in the ring: no straight write past it
  c.final();
  TEST_ASSERT_EQUAL_UINT32(3812, drainStep());
  TEST_ASSERT_EQUAL_UINT32(0, c.written() % FlashSteps::kSector);
  while (c.used()) {
    const uint32_t before = c.written();
    const uint32_t n = drainStep();
    TEST_ASSERT_EQUAL_UINT32(0, before % FlashSteps::kSector);
    TEST_ASSERT_LESS_OR_EQUAL(1, FlashSteps::erases(before, n));
  }
  TEST_ASSERT_EQUAL_UINT32(24380, c.written());
}

static void test_ring_wraps_and_keeps_order(void) {
  TEST_ASSERT_TRUE(c.begin(100, 0));
  useRing(10000);
  c.opened(true);
  uint8_t next = 0;
  uint8_t expect = 0;
  auto pushSeq = [&](uint32_t len) {
    Span s[2];
    TEST_ASSERT_TRUE(c.reserve(len, s));
    for (int k = 0; k < 2; ++k)
      for (uint32_t i = 0; i < s[k].len; ++i) s[k].data[i] = next++;
    c.commit(len);
  };
  auto drainSeq = [&]() {
    Span s[2];
    uint32_t len = 0;
    TEST_ASSERT_TRUE(c.next(s, len) == A::Write);
    for (int k = 0; k < 2; ++k)
      for (uint32_t i = 0; i < s[k].len; ++i) TEST_ASSERT_EQUAL_UINT8(expect++, s[k].data[i]);
    c.wrote(len, true, true);
    c.done();
    return len;
  };
  pushSeq(6000);
  TEST_ASSERT_EQUAL_UINT32(4096, drainSeq());
  Span s[2];
  TEST_ASSERT_TRUE(c.reserve(7000, s));  // wraps: 4000 at the end, 3000 at the start
  TEST_ASSERT_EQUAL_UINT32(4000, s[0].len);
  TEST_ASSERT_EQUAL_UINT32(3000, s[1].len);
  TEST_ASSERT_TRUE(s[1].data == ring.data());
  for (int k = 0; k < 2; ++k)
    for (uint32_t i = 0; i < s[k].len; ++i) s[k].data[i] = next++;
  c.commit(7000);
  c.final();
  while (c.used()) drainSeq();
  TEST_ASSERT_EQUAL_UINT32(13000, c.written());
}

static void test_window_holds_and_reopens(void) {
  TEST_ASSERT_TRUE(c.begin(1000, 0));
  useRing(6000);
  c.opened(true);
  TEST_ASSERT_TRUE(push(4500));
  TEST_ASSERT_TRUE(c.window() == W::Open);  // 1500 free
  TEST_ASSERT_TRUE(push(600));
  TEST_ASSERT_TRUE(c.window() == W::Hold);  // 900 free
  TEST_ASSERT_EQUAL_UINT32(1, c.holds());
  TEST_ASSERT_TRUE(c.window() == W::Hold);  // held: every segment
  Span s[2];
  uint32_t len;
  TEST_ASSERT_TRUE(c.next(s, len) == A::Write);
  c.wrote(200, true, true);  // the sink took part of it
  c.done();
  TEST_ASSERT_TRUE(c.window() == W::Reopen);  // 1100 free
  TEST_ASSERT_TRUE(c.window() == W::Open);
  TEST_ASSERT_EQUAL_UINT32(1, c.holds());
  // A failure lets the rest of the body flow.
  TEST_ASSERT_TRUE(push(500));
  TEST_ASSERT_TRUE(c.window() == W::Hold);
  c.fail(UploadResult::SinkFailed);
  TEST_ASSERT_TRUE(c.window() == W::Reopen);
  TEST_ASSERT_TRUE(c.window() == W::Open);
}

static void test_window_without_ring_never_holds(void) {
  TEST_ASSERT_TRUE(c.begin(1000, 0));
  TEST_ASSERT_TRUE(c.window() == W::Open);
  Span s[2];
  TEST_ASSERT_FALSE(c.reserve(10, s));  // no ring: nothing kept (and no failure)
  TEST_ASSERT_FALSE(c.failed());
}

// lwIP's window model: the peer may have at most TCP_WND bytes the device
// has not acknowledged (arrived, queued, or held by ackLater). Segments of
// up to one MSS; the parser may miss one held segment per hold (acked
// anyway); acks also come from a poll. The ring must never overflow.
static void test_tcp_window_never_overflows(void) {
  const uint32_t kWnd = 5760, kMss = 1436;
  const uint32_t holdBelow = kWnd + 2 * kMss;
  const uint32_t size = FlashSteps::kSector + holdBelow;
  TEST_ASSERT_TRUE(c.begin(holdBelow, 0));
  useRing(size);
  c.opened(true);
  uint32_t seed = 12345;
  auto rnd = [&](uint32_t n) {
    seed = seed * 1103515245u + 12345u;
    return (seed >> 8) % n;
  };
  std::deque<uint32_t> inFlight;  // sent, not yet processed
  uint32_t inFlightBytes = 0;
  uint32_t unacked = 0;  // processed, held by ackLater
  bool slipped = false;
  uint64_t sent = 0, drained = 0;
  uint32_t maxUsed = 0;
  for (int it = 0; it < 400000; ++it) {
    const uint32_t r = rnd(100);
    if (r < 45) {  // the peer sends what the window allows
      const uint32_t seg = 1 + rnd(kMss);
      if (inFlightBytes + unacked + seg <= kWnd) {
        inFlight.push_back(seg);
        inFlightBytes += seg;
        sent += seg;
      }
    } else if (r < 80) {  // async_tcp processes one segment
      if (inFlight.empty()) continue;
      const uint32_t seg = inFlight.front();
      inFlight.pop_front();
      inFlightBytes -= seg;
      TEST_ASSERT_TRUE_MESSAGE(push(seg), "ring overflow");
      const W w = c.window();
      if (w == W::Hold) {
        if (!slipped && rnd(50) == 0) {
          slipped = true;  // the parser kept it: acknowledged anyway
        } else {
          unacked += seg;
        }
      } else {
        if (w == W::Reopen) unacked = 0;
        slipped = w == W::Hold ? slipped : false;
      }
    } else if (r < 83) {  // the poll
      if (c.window() == W::Reopen) {
        unacked = 0;
        slipped = false;
      }
    } else if (r < 85u + uint32_t(it / 50000 % 2) * 10u) {  // the writer: slow, then fast
      if (c.pending()) drained += drainStep();
    }
    if (c.used() > maxUsed) maxUsed = c.used();
  }
  TEST_ASSERT_FALSE(c.failed());
  TEST_ASSERT_GREATER_THAN(10u, c.holds());
  TEST_ASSERT_GREATER_THAN(size - holdBelow, maxUsed);  // it did get full
  TEST_ASSERT_TRUE(drained + c.used() + inFlightBytes == sent);
}

static void test_sink_failure_aborts(void) {
  TEST_ASSERT_TRUE(c.begin(1000, 0));
  useRing(16384);
  Span s[2];
  uint32_t len;
  TEST_ASSERT_TRUE(c.next(s, len) == A::Open);
  c.opened(true);
  c.done();
  TEST_ASSERT_TRUE(push(4096));
  TEST_ASSERT_TRUE(c.next(s, len) == A::Write);
  c.wrote(len, false, true);
  TEST_ASSERT_TRUE(c.failed());
  TEST_ASSERT_TRUE(c.result() == UploadResult::SinkFailed);
  TEST_ASSERT_TRUE(c.takeRing() == nullptr);  // the step still holds the sink
  c.done();
  TEST_ASSERT_FALSE(c.settled());
  TEST_ASSERT_TRUE(c.takeRing() == ring.data());  // failed, nobody uses it
  TEST_ASSERT_FALSE(c.accepting());
  TEST_ASSERT_FALSE(push(10));
  TEST_ASSERT_TRUE(c.next(s, len) == A::Abort);
  c.aborted();
  c.done();
  TEST_ASSERT_TRUE(c.settled());
  TEST_ASSERT_TRUE(c.next(s, len) == A::None);
  c.fail(UploadResult::Aborted);  // the client leaves afterwards: the first failure stays
  TEST_ASSERT_TRUE(c.result() == UploadResult::SinkFailed);
}

// The sink may hold something from before open (OtaWriter::reserve): a
// failed session always ends with its abort.
static void test_failure_before_open_still_aborts(void) {
  TEST_ASSERT_TRUE(c.begin(1000, 0));
  c.fail(UploadResult::Aborted);
  TEST_ASSERT_FALSE(c.settled());
  TEST_ASSERT_FALSE(c.canBegin());
  Span s[2];
  uint32_t len;
  TEST_ASSERT_TRUE(c.pending());
  TEST_ASSERT_TRUE(c.next(s, len) == A::Abort);
  c.aborted();
  c.done();
  TEST_ASSERT_TRUE(c.settled());
  TEST_ASSERT_TRUE(c.next(s, len) == A::None);
  // Open fails: the abort follows too.
  TEST_ASSERT_TRUE(c.begin(1000, 0));
  TEST_ASSERT_TRUE(c.result() == UploadResult::Ok);
  TEST_ASSERT_TRUE(c.next(s, len) == A::Open);
  c.opened(false);
  c.done();
  TEST_ASSERT_FALSE(c.settled());
  TEST_ASSERT_TRUE(c.next(s, len) == A::Abort);
  c.aborted();
  c.done();
  TEST_ASSERT_TRUE(c.settled());
  TEST_ASSERT_TRUE(c.result() == UploadResult::SinkFailed);
}

static void test_client_gone_mid_step_aborts_after_it(void) {
  TEST_ASSERT_TRUE(c.begin(1000, 0));
  useRing(16384);
  Span s[2];
  uint32_t len;
  c.next(s, len);
  c.opened(true);
  c.done();
  TEST_ASSERT_TRUE(push(5000));
  TEST_ASSERT_TRUE(c.next(s, len) == A::Write);
  c.fail(UploadResult::Aborted);  // async_tcp: the client left
  TEST_ASSERT_FALSE(c.settled());
  c.wrote(len, true, true);
  c.done();
  TEST_ASSERT_TRUE(c.next(s, len) == A::Abort);
  c.aborted();
  c.done();
  TEST_ASSERT_TRUE(c.settled());
  TEST_ASSERT_TRUE(c.result() == UploadResult::Aborted);
}

static void test_client_gone_during_last_finish_call(void) {
  TEST_ASSERT_TRUE(c.begin(1000, 0));
  Span s[2];
  uint32_t len;
  c.next(s, len);
  c.opened(true);
  c.done();
  c.final();
  TEST_ASSERT_TRUE(c.next(s, len) == A::Finish);
  c.finished(UploadSink::kMore);
  c.done();
  TEST_ASSERT_TRUE(c.next(s, len) == A::Finish);
  c.fail(UploadResult::Aborted);
  c.finished(UploadSink::kDone);  // committed: nothing to abort
  c.done();
  TEST_ASSERT_TRUE(c.settled());
  TEST_ASSERT_TRUE(c.result() == UploadResult::Aborted);
  TEST_ASSERT_TRUE(c.next(s, len) == A::None);
}

// A failure between two finish calls (the client leaves, a stall): the
// finish goes on to its end, the failure is the result.
static void test_failure_between_finish_calls_lets_finish_complete(void) {
  TEST_ASSERT_TRUE(c.begin(1000, 0));
  Span s[2];
  uint32_t len;
  c.next(s, len);
  c.opened(true);
  c.done();
  c.final();
  TEST_ASSERT_TRUE(c.next(s, len) == A::Finish);
  TEST_ASSERT_TRUE(c.finishing());
  c.finished(UploadSink::kMore);  // e.g. the old file removed
  c.done();
  c.fail(UploadResult::Aborted);  // async_tcp: the client left
  TEST_ASSERT_FALSE(c.settled());
  TEST_ASSERT_TRUE(c.pending());
  TEST_ASSERT_TRUE(c.next(s, len) == A::Finish);  // not Abort
  c.finished(UploadSink::kMore);
  c.done();
  c.fail(UploadResult::Stalled);  // the first failure stays
  TEST_ASSERT_TRUE(c.next(s, len) == A::Finish);
  c.finished(UploadSink::kDone);  // .partial -> target
  c.done();
  TEST_ASSERT_TRUE(c.settled());
  TEST_ASSERT_FALSE(c.finishing());
  TEST_ASSERT_TRUE(c.result() == UploadResult::Aborted);
  TEST_ASSERT_TRUE(c.next(s, len) == A::None);
  TEST_ASSERT_TRUE(c.canBegin());
}

// A finish call that fails after an earlier failure: then the abort.
static void test_finish_failing_after_failure_aborts(void) {
  TEST_ASSERT_TRUE(c.begin(1000, 0));
  Span s[2];
  uint32_t len;
  c.next(s, len);
  c.opened(true);
  c.done();
  c.final();
  TEST_ASSERT_TRUE(c.next(s, len) == A::Finish);
  c.finished(UploadSink::kMore);
  c.done();
  c.fail(UploadResult::Aborted);
  TEST_ASSERT_TRUE(c.next(s, len) == A::Finish);
  c.finished(UploadSink::kFailed);
  c.done();
  TEST_ASSERT_FALSE(c.finishing());
  TEST_ASSERT_TRUE(c.next(s, len) == A::Abort);
  c.aborted();
  c.done();
  TEST_ASSERT_TRUE(c.settled());
  TEST_ASSERT_TRUE(c.result() == UploadResult::Aborted);
}

// Failed before the finish began: the abort, as before.
static void test_failure_before_finish_aborts(void) {
  TEST_ASSERT_TRUE(c.begin(1000, 0));
  Span s[2];
  uint32_t len;
  c.next(s, len);
  c.opened(true);
  c.done();
  c.final();
  c.fail(UploadResult::Aborted);
  TEST_ASSERT_TRUE(c.next(s, len) == A::Abort);
  TEST_ASSERT_FALSE(c.finishing());
  c.aborted();
  c.done();
  TEST_ASSERT_TRUE(c.settled());
}

static void test_finish_failure_aborts(void) {
  TEST_ASSERT_TRUE(c.begin(1000, 0));
  Span s[2];
  uint32_t len;
  c.next(s, len);
  c.opened(true);
  c.done();
  c.final();
  TEST_ASSERT_TRUE(c.next(s, len) == A::Finish);
  c.finished(UploadSink::kFailed);
  c.done();
  TEST_ASSERT_TRUE(c.next(s, len) == A::Abort);
  c.aborted();
  c.done();
  TEST_ASSERT_TRUE(c.settled());
  TEST_ASSERT_TRUE(c.result() == UploadResult::SinkFailed);
}

static void test_claim_and_next_exclude_each_other(void) {
  TEST_ASSERT_TRUE(c.begin(1000, 0));
  useRing(16384);
  TEST_ASSERT_TRUE(c.claim());
  Span s[2];
  uint32_t len;
  TEST_ASSERT_TRUE(c.next(s, len) == A::None);
  TEST_ASSERT_FALSE(c.claim());
  c.opened(true);
  c.done();
  // No claim while the producer copies.
  TEST_ASSERT_TRUE(c.reserve(10, s));
  TEST_ASSERT_FALSE(c.claim());
  c.commit(10);
  TEST_ASSERT_FALSE(c.claim());  // data in the ring goes first
  TEST_ASSERT_TRUE(c.next(s, len) == A::None);  // less than a step: waits for more
  c.final();
  TEST_ASSERT_TRUE(c.next(s, len) == A::Write);
  TEST_ASSERT_EQUAL_UINT32(10, len);
  TEST_ASSERT_FALSE(c.claim());
  c.wrote(len, true, true);
  c.done();
  TEST_ASSERT_TRUE(c.claim());  // empty again
  c.done();
}

static void test_ring_taken_back_only_when_unused(void) {
  TEST_ASSERT_TRUE(c.begin(1000, 0));
  useRing(16384);
  TEST_ASSERT_TRUE(c.takeRing() == nullptr);  // running
  Span s[2];
  TEST_ASSERT_TRUE(c.reserve(100, s));
  c.fail(UploadResult::Aborted);
  TEST_ASSERT_TRUE(c.takeRing() == nullptr);  // the producer is copying into it
  c.commit(100);
  TEST_ASSERT_TRUE(c.takeRing() == ring.data());
  TEST_ASSERT_FALSE(c.hasRing());
  TEST_ASSERT_TRUE(c.takeRing() == nullptr);
}

static void test_overflow_fails_the_upload(void) {
  TEST_ASSERT_TRUE(c.begin(100, 0));
  useRing(1000);
  TEST_ASSERT_TRUE(push(900));
  TEST_ASSERT_FALSE(push(200));
  TEST_ASSERT_TRUE(c.result() == UploadResult::Overflow);
  TEST_ASSERT_FALSE(c.settled());  // never opened: the abort still runs
  Span s[2];
  uint32_t len;
  TEST_ASSERT_TRUE(c.next(s, len) == A::Abort);
  c.aborted();
  c.done();
  TEST_ASSERT_TRUE(c.settled());
}

static void test_begin_refused_while_running(void) {
  TEST_ASSERT_TRUE(c.begin(1000, 0));
  TEST_ASSERT_FALSE(c.begin(1000, 0));
  useRing(4096);
  c.fail(UploadResult::Aborted);
  TEST_ASSERT_FALSE(c.begin(1000, 0));  // the abort first
  Span s[2];
  uint32_t len;
  TEST_ASSERT_TRUE(c.next(s, len) == A::Abort);
  c.aborted();
  c.done();
  TEST_ASSERT_TRUE(c.begin(1000, 0));  // settled: a new one, the ring kept for it
  TEST_ASSERT_TRUE(c.hasRing());
  TEST_ASSERT_TRUE(c.result() == UploadResult::Ok);
  TEST_ASSERT_TRUE(c.next(s, len) == A::Open);  // busy
  c.fail(UploadResult::Aborted);
  TEST_ASSERT_FALSE(c.canBegin());
  c.opened(false);
  c.done();
  TEST_ASSERT_FALSE(c.canBegin());  // the abort is due
  TEST_ASSERT_TRUE(c.next(s, len) == A::Abort);
  c.aborted();
  c.done();
  TEST_ASSERT_TRUE(c.canBegin());
}

static void test_empty_upload(void) {
  TEST_ASSERT_TRUE(c.begin(1000, 0));
  c.final();
  Span s[2];
  uint32_t len;
  TEST_ASSERT_TRUE(c.next(s, len) == A::Open);
  c.opened(true);
  c.done();
  TEST_ASSERT_TRUE(c.next(s, len) == A::Finish);
  c.finished(UploadSink::kDone);
  c.done();
  TEST_ASSERT_TRUE(c.settled());
  TEST_ASSERT_TRUE(c.result() == UploadResult::Ok);
  TEST_ASSERT_EQUAL_UINT32(0, c.written());
}

static void test_stall_clock_wraps(void) {
  const uint32_t t0 = 0xFFFFF000u;
  TEST_ASSERT_TRUE(c.begin(1000, t0));
  TEST_ASSERT_FALSE(c.stalled(t0 + 29999u, 30000));
  TEST_ASSERT_TRUE(c.stalled(t0 + 30000u, 30000));
  c.noteProgress(t0 + 20000u);
  TEST_ASSERT_FALSE(c.stalled(t0 + 49999u, 30000));
  TEST_ASSERT_TRUE(c.stalled(t0 + 50000u, 30000));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_flash_steps);
  RUN_TEST(test_session_order);
  RUN_TEST(test_steps_realign_after_direct_writes);
  RUN_TEST(test_ring_wraps_and_keeps_order);
  RUN_TEST(test_window_holds_and_reopens);
  RUN_TEST(test_window_without_ring_never_holds);
  RUN_TEST(test_tcp_window_never_overflows);
  RUN_TEST(test_sink_failure_aborts);
  RUN_TEST(test_failure_before_open_still_aborts);
  RUN_TEST(test_client_gone_mid_step_aborts_after_it);
  RUN_TEST(test_client_gone_during_last_finish_call);
  RUN_TEST(test_failure_between_finish_calls_lets_finish_complete);
  RUN_TEST(test_finish_failing_after_failure_aborts);
  RUN_TEST(test_failure_before_finish_aborts);
  RUN_TEST(test_finish_failure_aborts);
  RUN_TEST(test_claim_and_next_exclude_each_other);
  RUN_TEST(test_ring_taken_back_only_when_unused);
  RUN_TEST(test_overflow_fails_the_upload);
  RUN_TEST(test_begin_refused_while_running);
  RUN_TEST(test_empty_upload);
  RUN_TEST(test_stall_clock_wraps);
  return UNITY_END();
}
