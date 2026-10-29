#pragma once
#ifndef ESPRACK_FS_WRITE_QUEUE_H
#define ESPRACK_FS_WRITE_QUEUE_H

// FsWriteQueue: the file manager's writes (a directory made, a file or a
// tree removed, a rename) through the flash write gate (power/WriteGate.h).
// The asker (async_tcp) never waits for the gate. While the gate lets the
// write in now (a free gate: always, as without the gate) it goes at once
// and the answer says how it went. Otherwise it is kept here and the
// FlashWriter task (power/FlashWriter.h) does it, a gate slot per step, in
// the order asked; the answer is QUEUED with a number for status().
// Nothing goes ahead of a write kept here.
//
// A step is one bounded flash operation: one directory made, one entry
// removed, one rename. A tree goes one removal per step, depth first; each
// step walks down again from the tree's root (reads only), so nothing of the
// walk is kept between steps and a step PowerGuard held just runs again.
// A tree that may go at once while a module holds write gate windows goes
// kFreeSteps at once at most, the rest is kept (no window should meet a
// long run of writes).
//
// The FlashWriter job does one step per call, each with a wait clock of its
// own (maxWaitMs bounds a step's wait, not the whole write's), and returns:
// the framework's other jobs (config saves, an upload's ring) go between
// the steps of a long tree.
//
// A restart: close() (the module's hook, never waits) and nothing new
// starts; shutdown() (the restart's flush, after every hook) lets the step
// under way end, then the kept writes go on while the bound lasts; the
// caller logs what is left.
//
// Arduino-free (host-tested). Io supplies:
//   using Vol, Str;                  a volume (fs::FS*), a path (String)
//   bool lock(uint32_t ms); void unlock();   guards the slots, with a timeout
//   bool stepLock(uint32_t ms); void stepUnlock();   held around steps
//   bool canDefer(); void kick();    the FlashWriter job (kick: run())
//   int8_t kind(Vol, path)           -1 none, 0 file, 1 directory
//   bool exists(Vol, path)
//   bool firstChild(Vol, dir, Str& child, bool& isDir)   its full path
//   bool mkdir / remove / rmdir(Vol, path); bool rename(Vol, from, to)

#include <stdint.h>

#include <atomic>

#include "power/FlashWriter.h"
#include "power/WriteGate.h"

enum class FsResult : uint8_t {
  OK,
  NOT_FOUND,
  NOT_MOUNTED,
  READ_ONLY,
  INVALID_PATH,
  IO_ERROR,
  EXISTS,
  QUEUED,  // put off by the write gate: the FlashWriter task does it
  BUSY,    // no room to keep it now: ask again
};

template <class Io>
class FsWriteQueue {
 public:
  using Vol = typename Io::Vol;
  using Str = typename Io::Str;

  enum class Kind : uint8_t { Mkdir, Remove, RemoveTree, Rename };
  // Now: going at once on the asker's task.
  enum class State : uint8_t { None, Now, Queued, Running, Done, Failed };
  // status(): Busy = the lock was not to be had (ask again), not "no such write".
  enum class Lookup : uint8_t { Found, Missing, Busy };

  static constexpr uint8_t kSlots = 8;       // writes kept, waiting or finished (for status())
  static constexpr uint32_t kFreeSteps = 4;  // a tree at once, while a module holds windows
  static constexpr uint32_t kLockMs = 50;    // the slots' lock: held for copies only
  static constexpr uint16_t kMaxDepth = 64;  // a walk deeper than this: the FS is not what we think

  struct Status {
    uint32_t id = 0;
    Kind kind = Kind::Mkdir;
    State state = State::None;
    FsResult result = FsResult::OK;
    uint32_t steps = 0;  // writes done
    Str path;
  };

  // firstId: where the numbers start (a random one per boot: a number
  // polled across a restart does not name another write).
  explicit FsWriteQueue(Io& io, uint32_t firstId = 0) : _io(io), _next(firstId) {}
  FsWriteQueue(const FsWriteQueue&) = delete;
  FsWriteQueue& operator=(const FsWriteQueue&) = delete;

  // Any task, never waits for the gate. The result of the write when it
  // went at once; QUEUED: kept (*id: its number); BUSY: all slots wait, the
  // lock was not to be had, nothing could run it later, or a restart began.
  // The asker checked the volume and the path; the steps check what exists.
  FsResult submit(Kind kind, Vol vol, const Str& path, const Str& to = Str(), uint32_t* id = nullptr) {
    if (id) *id = 0;
    if (!_io.lock(kLockMs)) return FsResult::BUSY;
    Op* op = _closed.load() ? nullptr : freeSlot();
    if (!op) {
      _io.unlock();
      return FsResult::BUSY;
    }
    const uint32_t now = ESPRack::WriteGate::now();
    Pass pass;
    // A write kept here goes first: a new one waits behind it.
    if (!pendingLocked()) pass.on = ESPRack::WriteGate::tryEnter(now, kind == Kind::RemoveTree, &pass.at);
    if (!pass.on && !_io.canDefer()) {
      _io.unlock();
      return FsResult::BUSY;
    }
    if (++_next == 0) _next = 1;
    op->id = _next;
    op->kind = kind;
    op->vol = vol;
    op->path = path;
    op->to = to;
    op->steps.store(0);
    op->result.store(static_cast<uint8_t>(FsResult::OK));
    op->state.store(static_cast<uint8_t>(pass.on ? State::Now : State::Queued));
    if (id) *id = op->id;
    _io.unlock();

    if (!pass.on) {
      ESPRack::WriteGate::noteDeferred();
      _io.kick();
      return FsResult::QUEUED;
    }
    // At once, on this pass. The slot is this task's while it is Now: its
    // paths are not written by anyone else meanwhile. The step lock is only
    // held long by a restart: then nothing is written.
    if (!_io.stepLock(kLockMs)) {
      pass.release();
      op->state.store(static_cast<uint8_t>(State::None));
      if (id) *id = 0;
      return FsResult::BUSY;
    }
    const uint32_t cap = (kind == Kind::RemoveTree && ESPRack::WriteGate::holders()) ? kFreeSteps : UINT32_MAX;
    uint32_t n = 0;
    FsResult r = FsResult::OK;
    bool more = false;
    do {
      more = advance(*op, r);
    } while (more && ++n < cap);
    _io.stepUnlock();
    pass.release();
    if (more) {  // the rest of a tree: kept, it keeps its place
      op->state.store(static_cast<uint8_t>(State::Queued));
      ESPRack::WriteGate::noteDeferred();
      _io.kick();
      return FsResult::QUEUED;
    }
    return r;
  }

  // The FlashWriter job: the oldest kept write, one step in a gate slot,
  // then back (1: run again at once, after the other jobs). The slots' lock
  // is never held while waiting for the gate or writing.
  uint32_t run() {
    if (_closed.load()) return ESPRack::FlashWriter::kDone;  // a restart: shutdown() has them
    bool busy = false;
    Op* op = take(busy);
    if (busy) return 20;  // run again soon
    if (!op) return ESPRack::FlashWriter::kDone;
    // A wait clock per step, from now: a long tree (or a write kept behind
    // one) never turns forced step after step.
    ESPRack::WriteSlot slot;
    if (!slot.ok()) return ESPRack::FlashWriter::kOnPower;  // PowerGuard paused writes: it stays
    if (!_io.stepLock(kLockMs)) return 20;
    if (!_closed.load()) {
      FsResult r;
      (void)advance(*op, r);
    }
    _io.stepUnlock();
    return 1;
  }

  // A restart began (any task, never waits): no new write, no new step;
  // the kept writes wait for shutdown().
  void close() { _closed.store(true); }

  // The restart's flush, after every module hook (its task may wait; no
  // module window is open any more; closes too). The step under way ends
  // first (boundMs at most), then the kept writes go on in order, a gate
  // slot per step, while boundMs lasts. What is left goes with the restart:
  // pending() counts it, forEachPending() lists it. The step lock stays
  // taken: nothing is written after this.
  void shutdown(uint32_t boundMs) {
    const uint32_t t0 = ESPRack::WriteGate::now();
    _closed.store(true);
    if (!_io.stepLock(boundMs)) return;  // a step still under way
    for (;;) {
      const uint32_t spent = ESPRack::WriteGate::now() - t0;
      if (spent >= boundMs) return;
      bool busy = false;
      Op* op = take(busy);
      if (!op) return;
      ESPRack::WriteSlot slot(boundMs - spent);
      if (!slot.ok()) return;  // PowerGuard paused writes: nothing goes
      FsResult r;
      (void)advance(*op, r);
    }
  }

  // Missing: no such write (any more); Busy: the lock was not to be had.
  Lookup status(uint32_t id, Status& out) {
    if (!id) return Lookup::Missing;
    if (!_io.lock(kLockMs)) return Lookup::Busy;
    Lookup found = Lookup::Missing;
    for (auto& o : _ops) {
      const State s = o.st();
      if (s == State::None || o.id != id) continue;
      copy(o, out);
      found = Lookup::Found;
      break;
    }
    _io.unlock();
    return found;
  }

  // Writes asked for and not finished (going now, kept or running).
  uint8_t pending() const {
    uint8_t n = 0;
    for (const auto& o : _ops) {
      if (o.live()) ++n;
    }
    return n;
  }

  // f(const Status&) for each write not finished, oldest first. false: the
  // lock was not to be had.
  template <class F>
  bool forEachPending(F f) {
    if (!_io.lock(kLockMs)) return false;
    Status list[kSlots];
    uint8_t n = 0;
    for (auto& o : _ops) {
      if (o.live()) copy(o, list[n++]);
    }
    _io.unlock();
    for (uint8_t i = 1; i < n; ++i) {  // a handful: insertion sort by number
      for (uint8_t j = i; j > 0 && int32_t(list[j].id - list[j - 1].id) < 0; --j) {
        Status t = list[j];
        list[j] = list[j - 1];
        list[j - 1] = t;
      }
    }
    for (uint8_t i = 0; i < n; ++i) f(list[i]);
    return true;
  }

  // One step of a write: one flash operation at most. more: a tree has
  // another step to go. done: the steps it already did (a tree whose root
  // is gone after them is removed).
  static FsResult step(Io& io, Kind kind, Vol vol, const Str& path, const Str& to, uint32_t done, bool& more) {
    more = false;
    switch (kind) {
      case Kind::Mkdir:
        if (io.exists(vol, path)) return FsResult::EXISTS;
        return io.mkdir(vol, path) ? FsResult::OK : FsResult::IO_ERROR;
      case Kind::Rename:
        if (!io.exists(vol, path)) return FsResult::NOT_FOUND;
        if (io.exists(vol, to)) return FsResult::EXISTS;
        return io.rename(vol, path, to) ? FsResult::OK : FsResult::IO_ERROR;
      case Kind::Remove: {
        const int8_t k = io.kind(vol, path);
        if (k < 0) return FsResult::NOT_FOUND;
        return (k ? io.rmdir(vol, path) : io.remove(vol, path)) ? FsResult::OK : FsResult::IO_ERROR;
      }
      case Kind::RemoveTree: {
        const int8_t k = io.kind(vol, path);
        if (k < 0) return done ? FsResult::OK : FsResult::NOT_FOUND;
        if (k == 0) return io.remove(vol, path) ? FsResult::OK : FsResult::IO_ERROR;
        // Down the first children to a file, or to an empty directory.
        Str dir = path;
        Str child;
        bool isDir = false;
        for (uint16_t depth = 0; io.firstChild(vol, dir, child, isDir); ++depth) {
          if (!isDir) {
            if (!io.remove(vol, child)) return FsResult::IO_ERROR;
            more = true;
            return FsResult::OK;
          }
          if (depth >= kMaxDepth) return FsResult::IO_ERROR;
          dir = child;
        }
        if (!io.rmdir(vol, dir)) return FsResult::IO_ERROR;
        more = !(dir == path);
        return FsResult::OK;
      }
    }
    return FsResult::INVALID_PATH;
  }

  // A whole tree at once (the caller holds the gate, or the volume is off
  // the code flash).
  static FsResult removeTree(Io& io, Vol vol, const Str& root) {
    bool more = false;
    uint32_t n = 0;
    FsResult r = FsResult::OK;
    do {
      r = step(io, Kind::RemoveTree, vol, root, Str(), n++, more);
    } while (r == FsResult::OK && more);
    return r;
  }

 private:
  // state, result and steps: plain loads and stores (the queue may live in
  // PSRAM). The slot's other fields are written only while it is free,
  // under the lock; while it is live only its owner (the asker while Now,
  // the step lock's holder after) writes state, result and steps.
  struct Op {
    uint32_t id = 0;
    Kind kind = Kind::Mkdir;
    Vol vol{};
    Str path;
    Str to;
    std::atomic<uint8_t> state{static_cast<uint8_t>(State::None)};
    std::atomic<uint8_t> result{static_cast<uint8_t>(FsResult::OK)};
    std::atomic<uint32_t> steps{0};
    State st() const { return static_cast<State>(state.load()); }
    bool live() const {
      const State s = st();
      return s == State::Now || s == State::Queued || s == State::Running;
    }
  };

  struct Pass {
    bool on = false;
    uint32_t at = 0;
    void release() {
      if (on) ESPRack::WriteGate::leave(at);
      on = false;
    }
    ~Pass() { release(); }
  };

  // The oldest kept write, marked Running; null when none, or (busy) when
  // the lock was not to be had.
  Op* take(bool& busy) {
    busy = !_io.lock(kLockMs);
    if (busy) return nullptr;
    Op* op = nullptr;
    for (auto& o : _ops) {
      const State s = o.st();
      if (s != State::Queued && s != State::Running) continue;
      if (!op || int32_t(o.id - op->id) < 0) op = &o;
    }
    if (op) op->state.store(static_cast<uint8_t>(State::Running));
    _io.unlock();
    return op;
  }

  // One step of op (the caller holds a pass or slot, and the step lock).
  // true: more to go; else it is finished with r.
  bool advance(Op& op, FsResult& r) {
    bool more = false;
    const uint32_t n = op.steps.load();
    r = step(_io, op.kind, op.vol, op.path, op.to, n, more);
    if (r == FsResult::OK) op.steps.store(n + 1);
    if (r == FsResult::OK && more) return true;
    finish(op, r);
    return false;
  }

  static void finish(Op& op, FsResult r) {
    op.result.store(static_cast<uint8_t>(r));
    op.state.store(static_cast<uint8_t>(r == FsResult::OK ? State::Done : State::Failed));
  }

  static void copy(const Op& o, Status& out) {
    out.id = o.id;
    out.kind = o.kind;
    out.state = o.st();
    out.result = static_cast<FsResult>(o.result.load());
    out.steps = o.steps.load();
    out.path = o.path;
  }

  bool pendingLocked() const { return pending() > 0; }

  // A free slot, else the oldest finished one; null when all are live.
  Op* freeSlot() {
    Op* best = nullptr;
    for (auto& o : _ops) {
      const State s = o.st();
      if (s == State::None) return &o;
      if (o.live()) continue;
      if (!best || int32_t(o.id - best->id) < 0) best = &o;
    }
    return best;
  }

  Io& _io;
  Op _ops[kSlots];
  uint32_t _next = 0;
  std::atomic<bool> _closed{false};  // a restart began (plain loads and stores)
};

#endif  // ESPRACK_FS_WRITE_QUEUE_H
