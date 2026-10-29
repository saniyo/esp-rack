#pragma once
#ifndef PresenceLogWriter_h
#define PresenceLogWriter_h

// The presence log file's writes through the flash write gate
// (power/WriteGate.h), apart from the file and the locks (Io), so the host
// tests run it. One step per gate pass or slot: an append of up to a
// batch, or the roll-over. One writer of the file at a time (Io::claim),
// taken only once the pass or slot is held and given back before it is
// left, so a writer waiting for the gate never shuts another one out.
//
//   append(row)    a shared task (async_tcp): a gate pass, never a wait;
//                  else the line waits in the queue for the FlashWriter job
//                  (no job: the line is lost, counted; never past the gate)
//   job()          the FlashWriter job: ONE batch per run, then the other
//                  jobs go (a fresh wait clock per batch)
//   flush(boundMs) shutdown: what is queued, 2 x boundMs at most
//
// Io: using Str; bool lock(uint32_t ms); void unlock(); bool canDefer();
// void kick(); bool claim(bool wait); void release(); bool waitWriter(ms);
// bool overCap(); void roll(); bool append(const Str&); void lost(size_t).
// claim(true) that fails asks the writer at it to signal its release()
// (waitWriter).

#include <stddef.h>
#include <stdint.h>

#include <utility>

#include <power/FlashWriter.h>
#include <power/PowerGuard.h>
#include <power/WriteGate.h>

#include "PresenceLogQueue.h"

template <class Io>
class PresenceLogWriter {
 public:
  using Str = typename Io::Str;
  enum class Step : uint8_t { Empty, More, Busy, OnPower, NoLock };
  static constexpr uint32_t kLockMs = 50;  // the queue lock: a deque operation at most
  static constexpr int kFlushSteps = 4;    // a roll-over and the batches of a full queue

  PresenceLogWriter(Io& io, PresenceLogQueue<Str>& q, size_t batchBytes) : _io(io), _q(q), _batch(batchBytes) {}

  void append(Str&& row) {
    if (!_io.canDefer()) {  // no queue / job: gate passes only, else the line is lost
      for (int i = 0; i < 2; ++i) {
        ESPRack::WritePass pass(ESPRack::WriteGate::now());
        if (!pass.ok()) break;
        if (_io.overCap()) {  // a step of its own; the line takes the next pass
          _io.roll();
          continue;
        }
        if (!_io.append(row)) _io.lost(1);
        return;
      }
      _io.lost(1);
      return;
    }
    // At once when nothing is queued, else behind the queued lines.
    if (!_io.lock(kLockMs)) {
      _io.lost(1);
      return;
    }
    const bool queued = !_q.empty();
    if (queued) _q.push(std::move(row));
    _io.unlock();
    if (queued) {
      _io.kick();
      return;
    }
    bool queue = true;
    {
      ESPRack::WritePass pass(ESPRack::WriteGate::now());
      if (!pass.ok()) {
        if (ESPRack::PowerGuard::writesAllowed()) ESPRack::WriteGate::noteDeferred();
      } else if (_io.claim(false)) {
        if (_io.overCap()) {
          _io.roll();  // the line goes to the job, a step of its own
        } else {
          if (!_io.append(row)) _io.lost(1);
          queue = false;
        }
        _io.release();
      }
      if (queue) {  // under the pass still: no writer woken by its end runs ahead of this line
        if (_io.lock(kLockMs)) {
          _q.push(std::move(row));
          _io.unlock();
        } else {
          _io.lost(1);
          queue = false;
        }
      }
    }
    if (queue) _io.kick();
  }

  // One step: the roll-over or a batch. boundMs: the slot's bound (0: the
  // gate's rules). wait: on Busy, the writer at it signals its release.
  Step drainOne(uint32_t boundMs, bool wait = false) {
    if (!_io.lock(kLockMs)) return Step::NoLock;
    bool empty = _q.empty();
    _io.unlock();
    if (empty) return Step::Empty;
    ESPRack::WriteSlot slot(boundMs);
    if (!slot.ok()) return Step::OnPower;  // PowerGuard: the lines stay queued
    if (!_io.claim(wait)) return Step::Busy;
    Step r = Step::More;
    if (_io.overCap()) {
      _io.roll();  // the batch goes in the next slot
    } else {
      // What this slot writes, taken now: lines that come meanwhile wait
      // in the queue (bounded), not in a batch that could be lost.
      Str batch;
      size_t n = 0;
      if (_io.lock(kLockMs)) {
        batch.reserve(_batch < _q.bytes() ? _batch : _q.bytes());
        n = _q.take(batch, _batch);
        _io.unlock();
      } else {
        r = Step::NoLock;
      }
      if (n && !_io.append(batch)) _io.lost(n);
    }
    _io.release();
    if (r == Step::More && _io.lock(kLockMs)) {
      empty = _q.empty();
      _io.unlock();
      if (empty) r = Step::Empty;
    }
    return r;
  }

  // The FlashWriter job's return value.
  uint32_t job() {
    switch (drainOne(0)) {
      case Step::Empty: return ESPRack::FlashWriter::kDone;
      case Step::More: return 1;  // the other jobs go first
      case Step::OnPower: return ESPRack::FlashWriter::kOnPower;
      case Step::Busy:            // a pass on async_tcp is at it
      case Step::NoLock: break;
    }
    return 10;
  }

  // Shutdown. Returns the lines left in the queue (kept there).
  size_t flush(uint32_t boundMs) {
    const uint32_t start = ESPRack::WriteGate::now();
    auto left = [&]() -> uint32_t {
      const uint32_t gone = ESPRack::WriteGate::now() - start;
      return gone < 2 * boundMs ? 2 * boundMs - gone : 0;
    };
    for (int i = 0; i < kFlushSteps; ++i) {
      uint32_t bound = 0;  // boundMs 0: the gate's rules
      if (boundMs) {
        bound = left() < boundMs ? left() : boundMs;
        if (!bound) break;
      }
      const Step s = drainOne(bound, true);
      if (s == Step::Empty) return 0;
      if (s == Step::OnPower || s == Step::NoLock) break;
      if (s == Step::Busy && !_io.waitWriter(boundMs ? left() : 1000)) break;
    }
    if (!_io.lock(kLockMs)) return 0;
    const size_t n = _q.lines();
    _io.unlock();
    return n;
  }

 private:
  Io& _io;
  PresenceLogQueue<Str>& _q;
  size_t _batch;
};

#endif  // PresenceLogWriter_h
