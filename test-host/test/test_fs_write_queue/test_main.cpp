// Host tests of the file manager's writes through the flash write gate
// (modules/filesystem FsWriteQueue) on a fake volume and the host
// WriteGate (one thread, a set clock).

#include <unity.h>

#include <map>
#include <string>
#include <vector>

#include <power/PowerGuard.h>
#include <power/WriteGate.h>

#include "../../../modules/filesystem/include/FsWriteQueue.h"
#include "WriteGateHost.h"

using namespace ESPRack;

// A volume: path -> is a directory. "/" always exists.
struct FakeIo {
  using Vol = int;
  using Str = std::string;
  std::map<std::string, bool> entries;
  int kicks = 0;
  int writes = 0;
  bool lockOk = true;
  bool stepLockOk = true;
  bool stepHeld = false;
  bool defer = true;
  std::string failOn;  // a write to this path fails
  int powerFailAfter = -1;  // PowerGuard stops writes after this many
  uint32_t msPerWrite = 0;  // each write moves the gate's clock on this much

  bool lock(uint32_t) { return lockOk; }
  void unlock() {}
  bool stepLock(uint32_t) {
    if (!stepLockOk || stepHeld) return false;
    stepHeld = true;
    return true;
  }
  void stepUnlock() { stepHeld = false; }
  void wrote() {
    ++writes;
    if (msPerWrite) writeGateHostSetNow(WriteGate::now() + msPerWrite);
  }
  bool canDefer() const { return defer; }
  void kick() { ++kicks; }

  int8_t kind(Vol, const Str& p) {
    if (p == "/") return 1;
    auto it = entries.find(p);
    return it == entries.end() ? -1 : (it->second ? 1 : 0);
  }
  bool exists(Vol v, const Str& p) { return kind(v, p) >= 0; }
  static std::string parent(const Str& p) {
    const size_t s = p.rfind('/');
    return s == 0 ? "/" : p.substr(0, s);
  }
  bool firstChild(Vol, const Str& dir, Str& child, bool& isDir) {
    for (auto& e : entries) {
      if (parent(e.first) == dir) {
        child = e.first;
        isDir = e.second;
        return true;
      }
    }
    return false;
  }
  bool hasChildren(const Str& dir) {
    Str c;
    bool d;
    return firstChild(0, dir, c, d);
  }
  bool mkdir(Vol v, const Str& p) {
    wrote();
    if (p == failOn || kind(v, parent(p)) != 1) return false;
    entries[p] = true;
    return true;
  }
  bool remove(Vol, const Str& p) {
    wrote();
    auto it = entries.find(p);
    if (p == failOn || it == entries.end() || it->second) return false;
    entries.erase(it);
    if (writes == powerFailAfter) PowerGuard::setSupply(SupplyState::PowerFail);
    return true;
  }
  bool rmdir(Vol, const Str& p) {
    wrote();
    auto it = entries.find(p);
    if (p == failOn || it == entries.end() || !it->second || hasChildren(p)) return false;
    entries.erase(it);
    return true;
  }
  bool rename(Vol, const Str& from, const Str& to) {
    wrote();
    auto it = entries.find(from);
    if (from == failOn || it == entries.end()) return false;
    const bool d = it->second;
    entries.erase(it);
    entries[to] = d;
    return true;
  }
};

using Q = FsWriteQueue<FakeIo>;
using K = Q::Kind;
using S = Q::State;
using L = Q::Lookup;

static FakeIo fake;

void setUp(void) {
  fake = FakeIo{};
  PowerGuard::resetForTest();
  writeGateHostReset();
  WriteGateConfig c;
  c.idleMs = 1000;
  WriteGate::configure(c);
}
void tearDown(void) {}

static void tree() {
  fake.entries = {{"/d", true},          {"/d/a", false},   {"/d/b", true},   {"/d/b/c", false},
                {"/d/b/e", true},      {"/d/b/e/f", false}, {"/d/z", false}, {"/keep", false}};
}

static Q::Status st(Q& q, uint32_t id) {
  Q::Status s;
  TEST_ASSERT_TRUE(q.status(id, s) == L::Found);
  return s;
}

// The FlashWriter calls run() again while it says 1 (one step per call).
// What it said last; *calls: how many calls.
static uint32_t drain(Q& q, uint32_t* calls = nullptr) {
  uint32_t r = 0, n = 0;
  do {
    r = q.run();
    ++n;
  } while (r == 1 && n < 10000);
  if (calls) *calls = n;
  return r;
}

// No module ever opened a window: every write goes at once, as without the gate.
static void test_free_gate_goes_at_once(void) {
  Q q(fake);
  uint32_t id = 0;
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/x", "", &id) == FsResult::OK);
  TEST_ASSERT_TRUE(fake.entries.count("/x") == 1);
  TEST_ASSERT_TRUE(st(q, id).state == S::Done);
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/x") == FsResult::EXISTS);
  TEST_ASSERT_TRUE(q.submit(K::Rename, 0, "/x", "/y") == FsResult::OK);
  TEST_ASSERT_TRUE(q.submit(K::Rename, 0, "/x", "/y") == FsResult::NOT_FOUND);
  TEST_ASSERT_TRUE(q.submit(K::Remove, 0, "/y") == FsResult::OK);
  TEST_ASSERT_TRUE(q.submit(K::Remove, 0, "/y") == FsResult::NOT_FOUND);
  tree();
  TEST_ASSERT_TRUE(q.submit(K::Remove, 0, "/d") == FsResult::IO_ERROR);  // not empty, not recursive
  TEST_ASSERT_TRUE(q.submit(K::RemoveTree, 0, "/d", "", &id) == FsResult::OK);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)fake.entries.size());
  TEST_ASSERT_TRUE(fake.entries.count("/keep") == 1);
  TEST_ASSERT_EQUAL_UINT32(7, st(q, id).steps);  // 4 files, 3 directories: one write each
  TEST_ASSERT_EQUAL_INT(0, fake.kicks);
  TEST_ASSERT_EQUAL_UINT32(0, WriteGate::stats().deferred);
  TEST_ASSERT_EQUAL_UINT8(0, q.pending());
}

// A window open: the write is kept, the FlashWriter job does it, one slot per step.
static void test_window_queues_then_job_runs(void) {
  Q q(fake);
  const int8_t h = WriteGate::addHolder("bus");
  WriteGate::open(h);
  tree();
  uint32_t id = 0;
  TEST_ASSERT_TRUE(q.submit(K::RemoveTree, 0, "/d", "", &id) == FsResult::QUEUED);
  TEST_ASSERT_TRUE(id != 0);
  TEST_ASSERT_EQUAL_INT(1, fake.kicks);
  TEST_ASSERT_EQUAL_INT(0, fake.writes);
  TEST_ASSERT_TRUE(st(q, id).state == S::Queued);
  TEST_ASSERT_EQUAL_UINT32(1, WriteGate::stats().deferred);
  const uint32_t grants = WriteGate::stats().grants;
  uint32_t calls = 0;
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, drain(q, &calls));
  TEST_ASSERT_EQUAL_UINT32(8, calls);  // one step per call, then nothing left
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)fake.entries.size());
  const Q::Status s = st(q, id);
  TEST_ASSERT_TRUE(s.state == S::Done);
  TEST_ASSERT_EQUAL_UINT32(7, s.steps);
  TEST_ASSERT_EQUAL_UINT32(grants + 7, WriteGate::stats().grants);  // a slot per removal
  TEST_ASSERT_EQUAL_INT(7, fake.writes);
}

// Kept writes run in the order asked; a new one waits behind them even when the gate frees.
static void test_order_kept(void) {
  Q q(fake);
  const int8_t h = WriteGate::addHolder("bus");
  WriteGate::open(h);
  uint32_t a = 0, b = 0, c = 0;
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/a", "", &a) == FsResult::QUEUED);
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/a/b", "", &b) == FsResult::QUEUED);
  WriteGate::close(h);
  writeGateHostSetNow(5000);  // long idle: free
  TEST_ASSERT_TRUE(WriteGate::isFree());
  TEST_ASSERT_TRUE(q.submit(K::Rename, 0, "/a/b", "/a/c", &c) == FsResult::QUEUED);
  TEST_ASSERT_EQUAL_UINT8(3, q.pending());
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, drain(q));
  TEST_ASSERT_TRUE(st(q, a).state == S::Done);
  TEST_ASSERT_TRUE(st(q, b).state == S::Done);
  TEST_ASSERT_TRUE(st(q, c).state == S::Done);
  TEST_ASSERT_TRUE(fake.entries.count("/a/c") == 1);
  TEST_ASSERT_TRUE(fake.entries.count("/a/b") == 0);
  // Empty again: at once.
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/q") == FsResult::OK);
}

// A step that fails at run time: the write ends in error with why; the next ones still run.
static void test_failure_reported(void) {
  Q q(fake);
  const int8_t h = WriteGate::addHolder("bus");
  WriteGate::open(h);
  fake.entries = {{"/a", true}};
  uint32_t a = 0, b = 0;
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/a", "", &a) == FsResult::QUEUED);  // exists by then
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/b", "", &b) == FsResult::QUEUED);
  drain(q);
  Q::Status s = st(q, a);
  TEST_ASSERT_TRUE(s.state == S::Failed);
  TEST_ASSERT_TRUE(s.result == FsResult::EXISTS);
  TEST_ASSERT_TRUE(st(q, b).state == S::Done);
  // A tree whose removal fails half way stops there.
  tree();
  fake.failOn = "/d/b/c";
  uint32_t t = 0;
  TEST_ASSERT_TRUE(q.submit(K::RemoveTree, 0, "/d", "", &t) == FsResult::QUEUED);
  drain(q);
  s = st(q, t);
  TEST_ASSERT_TRUE(s.state == S::Failed);
  TEST_ASSERT_TRUE(s.result == FsResult::IO_ERROR);
  TEST_ASSERT_TRUE(fake.entries.count("/d/b/c") == 1);
}

// While a module holds windows, a tree that may go at once goes kFreeSteps at once, the rest is kept.
static void test_tree_capped_while_holders(void) {
  Q q(fake);
  const int8_t h = WriteGate::addHolder("bus");
  WriteGate::open(h);
  WriteGate::close(h);
  writeGateHostSetNow(5000);
  TEST_ASSERT_TRUE(WriteGate::isFree());
  tree();
  uint32_t id = 0;
  TEST_ASSERT_TRUE(q.submit(K::RemoveTree, 0, "/d", "", &id) == FsResult::QUEUED);
  TEST_ASSERT_EQUAL_INT(Q::kFreeSteps, fake.writes);
  TEST_ASSERT_EQUAL_UINT32(Q::kFreeSteps, st(q, id).steps);
  TEST_ASSERT_EQUAL_INT(1, fake.kicks);
  TEST_ASSERT_EQUAL_UINT8(0, WriteGate::writing());  // the pass is given back
  TEST_ASSERT_FALSE(fake.stepHeld);
  drain(q);
  TEST_ASSERT_TRUE(st(q, id).state == S::Done);
  TEST_ASSERT_EQUAL_UINT32(7, st(q, id).steps);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)fake.entries.size());
}

// One step at once goes into a pause between windows; a tree needs a free gate.
static void test_pause_takes_one_step(void) {
  Q q(fake);
  const int8_t h = WriteGate::addHolder("bus");
  WriteGate::open(h);
  writeGateHostSetNow(100);
  WriteGate::close(h, 200);
  writeGateHostSetNow(110);
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/p") == FsResult::OK);
  tree();
  TEST_ASSERT_TRUE(q.submit(K::RemoveTree, 0, "/d") == FsResult::QUEUED);
}

// PowerGuard pauses writes: kept, run() says kOnPower, nothing lost; done when allowed.
static void test_power_pause_keeps(void) {
  Q q(fake);
  PowerGuard::setSupply(SupplyState::PowerFail);
  uint32_t id = 0;
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/m", "", &id) == FsResult::QUEUED);
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kOnPower, q.run());
  TEST_ASSERT_FALSE(fake.stepHeld);
  TEST_ASSERT_TRUE(st(q, id).state == S::Running);
  TEST_ASSERT_EQUAL_INT(0, fake.writes);
  PowerGuard::setSupply(SupplyState::AcOk);
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, drain(q));
  TEST_ASSERT_TRUE(st(q, id).state == S::Done);
  // A tree PowerGuard stops half way walks again from its root.
  tree();
  const int8_t h = WriteGate::addHolder("bus");
  WriteGate::open(h);
  fake.writes = 0;
  fake.powerFailAfter = 3;
  TEST_ASSERT_TRUE(q.submit(K::RemoveTree, 0, "/d", "", &id) == FsResult::QUEUED);
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kOnPower, drain(q));
  TEST_ASSERT_EQUAL_UINT32(3, st(q, id).steps);
  TEST_ASSERT_TRUE(st(q, id).state == S::Running);
  PowerGuard::setSupply(SupplyState::AcOk);
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, drain(q));
  TEST_ASSERT_TRUE(st(q, id).state == S::Done);
  TEST_ASSERT_EQUAL_UINT32(7, st(q, id).steps);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)fake.entries.size());
}

// No room, no lock, or no job to run it later: BUSY, nothing written.
static void test_busy(void) {
  Q q(fake);
  const int8_t h = WriteGate::addHolder("bus");
  WriteGate::open(h);
  uint32_t ids[Q::kSlots] = {};
  for (uint8_t i = 0; i < Q::kSlots; ++i) {
    TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/n" + std::to_string(i), "", &ids[i]) == FsResult::QUEUED);
  }
  uint32_t id = 7;
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/over", "", &id) == FsResult::BUSY);
  TEST_ASSERT_EQUAL_UINT32(0, id);
  drain(q);
  // Finished ones make room, the oldest first.
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/over", "", &id) == FsResult::QUEUED);
  Q::Status s;
  TEST_ASSERT_TRUE(q.status(ids[0], s) == L::Missing);
  TEST_ASSERT_TRUE(q.status(ids[1], s) == L::Found);
  drain(q);
  fake.lockOk = false;
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/l") == FsResult::BUSY);
  fake.lockOk = true;
  fake.defer = false;
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/l") == FsResult::BUSY);
  WriteGate::close(h);
  writeGateHostSetNow(5000);
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/l") == FsResult::OK);  // free: no job needed
}

// The gate's clock wraps past 2^32 while a write is kept.
static void test_clock_wraps(void) {
  Q q(fake);
  writeGateHostSetNow(0xFFFFFF00u);
  const int8_t h = WriteGate::addHolder("bus");
  WriteGate::open(h);
  uint32_t id = 0;
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/w", "", &id) == FsResult::QUEUED);
  writeGateHostSetNow(0x80u);
  WriteGate::close(h, 500);
  writeGateHostSetNow(0x90u);
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, drain(q));
  TEST_ASSERT_TRUE(st(q, id).state == S::Done);
  TEST_ASSERT_EQUAL_UINT32(0, WriteGate::stats().forced);
}

// The whole tree at once (a caller holding the gate, or SD).
static void test_remove_tree_static(void) {
  tree();
  TEST_ASSERT_TRUE(Q::removeTree(fake, 0, "/d") == FsResult::OK);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)fake.entries.size());
  TEST_ASSERT_TRUE(Q::removeTree(fake, 0, "/d") == FsResult::NOT_FOUND);
  fake.entries = {{"/f", false}};
  TEST_ASSERT_TRUE(Q::removeTree(fake, 0, "/f") == FsResult::OK);
  TEST_ASSERT_TRUE(fake.entries.empty());
}

// Each step waits on a clock of its own: a write kept long ago is not
// "due" (or forced) for every step that follows (review F1).
static void test_step_wait_clock(void) {
  Q q(fake);
  writeGateHostSetNow(100);
  const int8_t h = WriteGate::addHolder("bus");
  WriteGate::open(h);
  tree();
  uint32_t id = 0;
  TEST_ASSERT_TRUE(q.submit(K::RemoveTree, 0, "/d", "", &id) == FsResult::QUEUED);
  writeGateHostSetNow(150);
  WriteGate::close(h, 500);
  // Long after: the bus kept the gate busy meanwhile.
  writeGateHostSetNow(19990);
  WriteGate::open(h);
  writeGateHostSetNow(20000);
  WriteGate::close(h, 500);
  writeGateHostSetNow(20010);
  WriteGate::resetStats();
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, drain(q));
  TEST_ASSERT_TRUE(st(q, id).state == S::Done);
  TEST_ASSERT_EQUAL_UINT32(7, st(q, id).steps);
  TEST_ASSERT_TRUE(WriteGate::stats().longestWaitMs < WriteGate::config().maxWaitMs);
}

// run() does one step and returns: the FlashWriter's other jobs go between
// the steps of a long tree (review F2).
static void test_one_step_per_call(void) {
  Q q(fake);
  const int8_t h = WriteGate::addHolder("bus");
  WriteGate::open(h);
  tree();
  uint32_t a = 0, b = 0;
  TEST_ASSERT_TRUE(q.submit(K::RemoveTree, 0, "/d", "", &a) == FsResult::QUEUED);
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/n", "", &b) == FsResult::QUEUED);
  for (int i = 1; i <= 7; ++i) {
    TEST_ASSERT_EQUAL_UINT32(1, q.run());
    TEST_ASSERT_EQUAL_INT(i, fake.writes);
    TEST_ASSERT_FALSE(fake.stepHeld);
  }
  TEST_ASSERT_TRUE(st(q, a).state == S::Done);
  TEST_ASSERT_TRUE(st(q, b).state == S::Queued);  // in order: after the tree
  TEST_ASSERT_EQUAL_UINT32(1, q.run());
  TEST_ASSERT_TRUE(st(q, b).state == S::Done);
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, q.run());
  // The step lock taken (a restart): no step, run again later.
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/o", "", &a) == FsResult::QUEUED);
  fake.stepLockOk = false;
  TEST_ASSERT_EQUAL_UINT32(20, q.run());
  TEST_ASSERT_EQUAL_INT(8, fake.writes);
  fake.stepLockOk = true;
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, drain(q));
  TEST_ASSERT_TRUE(st(q, a).state == S::Done);
}

// A restart: no new write, run() does nothing more; the kept writes go on
// in order while the bound lasts, the rest is listed (review F3).
static void test_shutdown(void) {
  {
    Q q(fake);
    const int8_t h = WriteGate::addHolder("bus");
    WriteGate::open(h);
    uint32_t a = 0, b = 0;
    TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/a", "", &a) == FsResult::QUEUED);
    TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/a/b", "", &b) == FsResult::QUEUED);
    q.shutdown(1000);
    TEST_ASSERT_TRUE(st(q, a).state == S::Done);
    TEST_ASSERT_TRUE(st(q, b).state == S::Done);
    TEST_ASSERT_EQUAL_UINT8(0, q.pending());
    TEST_ASSERT_TRUE(fake.stepHeld);  // kept: nothing steps after this
    uint32_t c = 9;
    TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/c", "", &c) == FsResult::BUSY);
    TEST_ASSERT_EQUAL_UINT32(0, c);
    TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, q.run());
    TEST_ASSERT_EQUAL_INT(2, fake.writes);
  }
  // The bound runs out half way: what is left stays, listed oldest first.
  fake = FakeIo{};
  writeGateHostReset();
  {
    Q q(fake);
    const int8_t h = WriteGate::addHolder("bus");
    WriteGate::open(h);
    tree();
    uint32_t t = 0, m = 0;
    TEST_ASSERT_TRUE(q.submit(K::RemoveTree, 0, "/d", "", &t) == FsResult::QUEUED);
    TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/m", "", &m) == FsResult::QUEUED);
    fake.msPerWrite = 400;
    q.shutdown(1000);
    TEST_ASSERT_EQUAL_INT(3, fake.writes);
    TEST_ASSERT_EQUAL_UINT8(2, q.pending());
    std::vector<uint32_t> seen;
    uint32_t steps = 0;
    TEST_ASSERT_TRUE(q.forEachPending([&](const Q::Status& s) {
      seen.push_back(s.id);
      if (s.id == t) steps = s.steps;
    }));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)seen.size());
    TEST_ASSERT_EQUAL_UINT32(t, seen[0]);
    TEST_ASSERT_EQUAL_UINT32(m, seen[1]);
    TEST_ASSERT_EQUAL_UINT32(3, steps);
  }
  // A step still under way past the bound: nothing more is written.
  fake = FakeIo{};
  writeGateHostReset();
  {
    Q q(fake);
    const int8_t h = WriteGate::addHolder("bus");
    WriteGate::open(h);
    TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/x") == FsResult::QUEUED);
    fake.stepLockOk = false;
    q.shutdown(1000);
    TEST_ASSERT_EQUAL_INT(0, fake.writes);
    TEST_ASSERT_EQUAL_UINT8(1, q.pending());
  }
}

// close() (the module's hook): nothing new starts and nothing is written,
// the kept writes stay for the restart's flush (shutdown, after every hook).
static void test_close_then_flush(void) {
  Q q(fake);
  const int8_t h = WriteGate::addHolder("bus");
  WriteGate::open(h);
  uint32_t a = 0;
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/a", "", &a) == FsResult::QUEUED);
  q.close();
  TEST_ASSERT_FALSE(fake.stepHeld);  // the hook never waits
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/c") == FsResult::BUSY);
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, q.run());
  TEST_ASSERT_EQUAL_INT(0, fake.writes);
  TEST_ASSERT_EQUAL_UINT8(1, q.pending());
  WriteGate::close(h);  // the hooks stopped the windows
  q.shutdown(1000);
  TEST_ASSERT_EQUAL_INT(1, fake.writes);
  TEST_ASSERT_TRUE(st(q, a).state == S::Done);
  TEST_ASSERT_EQUAL_UINT8(0, q.pending());
}

// status(): a lock not had is "busy", not "no such write" (review F5);
// numbers start where asked and skip 0 when they wrap; the order holds.
static void test_status_and_numbers(void) {
  Q q(fake, 0xFFFFFFFEu);
  const int8_t h = WriteGate::addHolder("bus");
  WriteGate::open(h);
  uint32_t a = 0, b = 0, c = 0;
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/a", "", &a) == FsResult::QUEUED);
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/a/b", "", &b) == FsResult::QUEUED);
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/a/b/c", "", &c) == FsResult::QUEUED);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, a);
  TEST_ASSERT_EQUAL_UINT32(1, b);
  TEST_ASSERT_EQUAL_UINT32(2, c);
  Q::Status s;
  fake.lockOk = false;
  TEST_ASSERT_TRUE(q.status(a, s) == L::Busy);
  fake.lockOk = true;
  TEST_ASSERT_TRUE(q.status(12345, s) == L::Missing);
  TEST_ASSERT_TRUE(q.status(0, s) == L::Missing);
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, drain(q));  // parents first: all made
  TEST_ASSERT_TRUE(st(q, c).state == S::Done);
  TEST_ASSERT_TRUE(fake.entries.count("/a/b/c") == 1);
}

// A write at once whose step lock is not to be had (a restart): BUSY, its
// slot given back, nothing written.
static void test_now_step_lock_busy(void) {
  Q q(fake);
  fake.stepLockOk = false;
  uint32_t id = 5;
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/x", "", &id) == FsResult::BUSY);
  TEST_ASSERT_EQUAL_UINT32(0, id);
  TEST_ASSERT_EQUAL_INT(0, fake.writes);
  TEST_ASSERT_EQUAL_UINT8(0, q.pending());
  TEST_ASSERT_EQUAL_UINT8(0, WriteGate::writing());
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_free_gate_goes_at_once);
  RUN_TEST(test_window_queues_then_job_runs);
  RUN_TEST(test_order_kept);
  RUN_TEST(test_failure_reported);
  RUN_TEST(test_tree_capped_while_holders);
  RUN_TEST(test_pause_takes_one_step);
  RUN_TEST(test_power_pause_keeps);
  RUN_TEST(test_busy);
  RUN_TEST(test_clock_wraps);
  RUN_TEST(test_remove_tree_static);
  RUN_TEST(test_step_wait_clock);
  RUN_TEST(test_one_step_per_call);
  RUN_TEST(test_shutdown);
  RUN_TEST(test_close_then_flush);
  RUN_TEST(test_status_and_numbers);
  RUN_TEST(test_now_step_lock_busy);
  return UNITY_END();
}
