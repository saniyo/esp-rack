// Host tests of the file manager's writes through the flash write gate
// (modules/filesystem FsWriteQueue) on a fake volume and the host
// WriteGate (one thread, a set clock).

#include <unity.h>

#include <map>
#include <string>

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
  bool defer = true;
  std::string failOn;  // a write to this path fails
  int powerFailAfter = -1;  // PowerGuard stops writes after this many

  bool lock(uint32_t) { return lockOk; }
  void unlock() {}
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
    ++writes;
    if (p == failOn || kind(v, parent(p)) != 1) return false;
    entries[p] = true;
    return true;
  }
  bool remove(Vol, const Str& p) {
    ++writes;
    auto it = entries.find(p);
    if (p == failOn || it == entries.end() || it->second) return false;
    entries.erase(it);
    if (writes == powerFailAfter) PowerGuard::setSupply(SupplyState::PowerFail);
    return true;
  }
  bool rmdir(Vol, const Str& p) {
    ++writes;
    auto it = entries.find(p);
    if (p == failOn || it == entries.end() || !it->second || hasChildren(p)) return false;
    entries.erase(it);
    return true;
  }
  bool rename(Vol, const Str& from, const Str& to) {
    ++writes;
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
  TEST_ASSERT_TRUE(q.status(id, s));
  return s;
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
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, q.run());
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
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, q.run());
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
  q.run();
  Q::Status s = st(q, a);
  TEST_ASSERT_TRUE(s.state == S::Failed);
  TEST_ASSERT_TRUE(s.result == FsResult::EXISTS);
  TEST_ASSERT_TRUE(st(q, b).state == S::Done);
  // A tree whose removal fails half way stops there.
  tree();
  fake.failOn = "/d/b/c";
  uint32_t t = 0;
  TEST_ASSERT_TRUE(q.submit(K::RemoveTree, 0, "/d", "", &t) == FsResult::QUEUED);
  q.run();
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
  q.run();
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
  TEST_ASSERT_TRUE(st(q, id).state == S::Running);
  TEST_ASSERT_EQUAL_INT(0, fake.writes);
  PowerGuard::setSupply(SupplyState::AcOk);
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, q.run());
  TEST_ASSERT_TRUE(st(q, id).state == S::Done);
  // A tree PowerGuard stops half way walks again from its root.
  tree();
  const int8_t h = WriteGate::addHolder("bus");
  WriteGate::open(h);
  fake.writes = 0;
  fake.powerFailAfter = 3;
  TEST_ASSERT_TRUE(q.submit(K::RemoveTree, 0, "/d", "", &id) == FsResult::QUEUED);
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kOnPower, q.run());
  TEST_ASSERT_EQUAL_UINT32(3, st(q, id).steps);
  TEST_ASSERT_TRUE(st(q, id).state == S::Running);
  PowerGuard::setSupply(SupplyState::AcOk);
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, q.run());
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
  q.run();
  // Finished ones make room, the oldest first.
  TEST_ASSERT_TRUE(q.submit(K::Mkdir, 0, "/over", "", &id) == FsResult::QUEUED);
  Q::Status s;
  TEST_ASSERT_FALSE(q.status(ids[0], s));
  TEST_ASSERT_TRUE(q.status(ids[1], s));
  q.run();
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
  TEST_ASSERT_EQUAL_UINT32(FlashWriter::kDone, q.run());
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
  return UNITY_END();
}
