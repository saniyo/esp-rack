// Host tests of the presence log's writes through the flash write gate
// (modules/presence PresenceLogWriter) on a fake file and the host
// WriteGate (one thread, a set clock; a slot that would wait goes forced).

#include <unity.h>

#include <string>

#include <power/PowerGuard.h>
#include <power/WriteGate.h>

#include "../../../modules/presence/include/PresenceLogWriter.h"
#include "WriteGateHost.h"

using namespace ESPRack;

struct FakeIo {
  using Str = std::string;
  std::string file;
  bool defer = true;
  bool lockOk = true;
  bool writing = false;  // another writer holds the file
  bool waits = false;    // claim(true) asked for a signal
  bool finishOnWait = true;  // waitWriter: that writer finishes (signals)
  size_t cap = 0;        // over cap once the file holds this many bytes (0: none)
  int kicks = 0, rolls = 0, appends = 0;
  size_t lostLines = 0;

  bool lock(uint32_t) { return lockOk; }
  void unlock() {}
  bool canDefer() const { return defer; }
  void kick() { ++kicks; }
  bool claim(bool wait) {
    if (writing) {
      if (wait) waits = true;
      return false;
    }
    writing = true;
    return true;
  }
  void release() { writing = false; }
  bool waitWriter(uint32_t ms) {
    if (!ms || !waits || !finishOnWait) return false;
    writing = waits = false;
    return true;
  }
  bool overCap() { return cap && file.size() >= cap; }
  void roll() {
    file.clear();
    ++rolls;
  }
  bool append(const std::string& d) {
    file += d;
    ++appends;
    return true;
  }
  void lost(size_t n) { lostLines += n; }
};

using W = PresenceLogWriter<FakeIo>;
using Q = PresenceLogQueue<std::string>;

static FakeIo fx;

void setUp(void) {
  fx = FakeIo{};
  PowerGuard::resetForTest();
  writeGateHostReset();
  WriteGateConfig c;
  c.idleMs = 1000;
  WriteGate::configure(c);
}
void tearDown(void) {}

static std::string row(char c) { return std::string(8, c) + "\r\n"; }  // 10 bytes

// No window ever: a line goes at once, as without the gate.
static void test_free_gate_at_once(void) {
  Q q(64, 4096);
  W w(fx, q, 4096);
  w.append(row('a'));
  TEST_ASSERT_TRUE(fx.file == row('a'));
  TEST_ASSERT_TRUE(q.empty());
  TEST_ASSERT_EQUAL_INT(0, fx.kicks);
  TEST_ASSERT_FALSE(fx.writing);
}

// In a window: queued in order, the job writes them after.
static void test_window_queues_job_writes(void) {
  Q q(64, 4096);
  W w(fx, q, 4096);
  const int8_t h = WriteGate::addHolder("bus");
  WriteGate::open(h);
  w.append(row('a'));
  w.append(row('b'));
  TEST_ASSERT_TRUE(fx.file.empty());
  TEST_ASSERT_EQUAL_UINT(2, q.lines());
  TEST_ASSERT_EQUAL_INT(2, fx.kicks);
  TEST_ASSERT_EQUAL_UINT32(1, WriteGate::stats().deferred);  // the second went behind the first
  WriteGate::close(h);
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, w.job());
  TEST_ASSERT_TRUE(fx.file == row('a') + row('b'));
  TEST_ASSERT_FALSE(fx.writing);
}

// One batch per job run: the other FlashWriter jobs go in between (S6).
static void test_job_one_batch_per_run(void) {
  Q q(64, 4096);
  W w(fx, q, 25);  // two lines per batch
  for (char c = 'a'; c < 'f'; ++c) q.push(row(c));
  TEST_ASSERT_EQUAL_UINT32(1, w.job());
  TEST_ASSERT_EQUAL_INT(1, fx.appends);
  TEST_ASSERT_EQUAL_UINT(3, q.lines());
  TEST_ASSERT_EQUAL_UINT32(1, w.job());
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, w.job());
  TEST_ASSERT_EQUAL_INT(3, fx.appends);
  TEST_ASSERT_TRUE(fx.file == row('a') + row('b') + row('c') + row('d') + row('e'));
}

// The job never holds the writer flag while it waits: here another writer
// holds it, the job looks again later and keeps the lines.
static void test_job_busy_keeps(void) {
  Q q(64, 4096);
  W w(fx, q, 4096);
  q.push(row('a'));
  fx.writing = true;
  TEST_ASSERT_EQUAL_UINT32(10, w.job());
  TEST_ASSERT_EQUAL_UINT(1, q.lines());
  TEST_ASSERT_FALSE(fx.waits);  // the job doesn't wait for it
}

// Shutdown: a writer at it is waited for (its end signalled), then the
// rest is written (S5).
static void test_flush_waits_for_writer(void) {
  Q q(64, 4096);
  W w(fx, q, 25);
  for (char c = 'a'; c < 'e'; ++c) q.push(row(c));
  fx.writing = true;
  TEST_ASSERT_EQUAL_UINT(0, w.flush(200));
  TEST_ASSERT_TRUE(fx.file == row('a') + row('b') + row('c') + row('d'));
  TEST_ASSERT_FALSE(fx.writing);
}

// ... and when that writer doesn't finish in time, the lines stay queued
// and flush says how many.
static void test_flush_gives_up_counts(void) {
  Q q(64, 4096);
  W w(fx, q, 4096);
  q.push(row('a'));
  q.push(row('b'));
  fx.writing = true;
  fx.finishOnWait = false;
  TEST_ASSERT_EQUAL_UINT(2, w.flush(200));
  TEST_ASSERT_EQUAL_UINT(2, q.lines());
}

// PowerGuard pauses writes: nothing written, nothing dropped, not counted
// as put off; the job waits for power, flush leaves them.
static void test_power_pause_keeps(void) {
  Q q(64, 4096);
  W w(fx, q, 4096);
  PowerGuard::setSupply(SupplyState::PowerFail);
  w.append(row('a'));
  TEST_ASSERT_TRUE(fx.file.empty());
  TEST_ASSERT_EQUAL_UINT(1, q.lines());
  TEST_ASSERT_EQUAL_UINT32(0, WriteGate::stats().deferred);
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kOnPower, w.job());
  TEST_ASSERT_EQUAL_UINT(1, w.flush(200));
  TEST_ASSERT_EQUAL_UINT(1, q.lines());
  TEST_ASSERT_EQUAL_UINT(0, fx.lostLines);
  PowerGuard::setSupply(SupplyState::AcOk);
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, w.job());
  TEST_ASSERT_TRUE(fx.file == row('a'));
}

// No FlashWriter job: never past the gate. Free: at once; in a window the
// line is lost and counted (S9).
static void test_no_job_never_past_gate(void) {
  Q q(64, 4096);
  W w(fx, q, 4096);
  fx.defer = false;
  w.append(row('a'));
  TEST_ASSERT_TRUE(fx.file == row('a'));
  const int8_t h = WriteGate::addHolder("bus");
  WriteGate::open(h);
  w.append(row('b'));
  TEST_ASSERT_TRUE(fx.file == row('a'));
  TEST_ASSERT_EQUAL_UINT(1, fx.lostLines);
  TEST_ASSERT_TRUE(q.empty());
  TEST_ASSERT_EQUAL_INT(0, fx.kicks);
}

// Over the cap: the roll-over is a step of its own, the line goes after.
static void test_roll_is_own_step(void) {
  Q q(64, 4096);
  W w(fx, q, 4096);
  fx.cap = 20;
  fx.file = std::string(20, 'x');
  w.append(row('a'));  // the pass rolls; the line to the job
  TEST_ASSERT_EQUAL_INT(1, fx.rolls);
  TEST_ASSERT_TRUE(fx.file.empty());
  TEST_ASSERT_EQUAL_UINT(1, q.lines());
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, w.job());
  TEST_ASSERT_TRUE(fx.file == row('a'));
  // The job too: a roll, then the batch in the next run.
  fx.file = std::string(20, 'x');
  q.push(row('b'));
  TEST_ASSERT_EQUAL_UINT32(1, w.job());
  TEST_ASSERT_EQUAL_INT(2, fx.rolls);
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, w.job());
  TEST_ASSERT_TRUE(fx.file == row('b'));
}

// A direct append that finds another writer at the file (a free gate lets
// several in) queues the line behind it instead.
static void test_direct_busy_queues(void) {
  Q q(64, 4096);
  W w(fx, q, 4096);
  fx.writing = true;
  w.append(row('a'));
  TEST_ASSERT_TRUE(fx.file.empty());
  TEST_ASSERT_EQUAL_UINT(1, q.lines());
  TEST_ASSERT_EQUAL_INT(1, fx.kicks);
  fx.writing = false;
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, w.job());
  TEST_ASSERT_TRUE(fx.file == row('a'));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_free_gate_at_once);
  RUN_TEST(test_window_queues_job_writes);
  RUN_TEST(test_job_one_batch_per_run);
  RUN_TEST(test_job_busy_keeps);
  RUN_TEST(test_flush_waits_for_writer);
  RUN_TEST(test_flush_gives_up_counts);
  RUN_TEST(test_power_pause_keeps);
  RUN_TEST(test_no_job_never_past_gate);
  RUN_TEST(test_roll_is_own_step);
  RUN_TEST(test_direct_busy_queues);
  return UNITY_END();
}
