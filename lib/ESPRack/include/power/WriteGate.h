#pragma once
#ifndef ESPRACK_WRITE_GATE_H
#define ESPRACK_WRITE_GATE_H

// WriteGate: when the flash may be written. A flash write or erase stops
// the caches while it runs (a 4 KB sector erase: ~30-50 ms, up to ~0.4 s):
// code in flash, data in PSRAM and with them the network wait. A module
// that needs the CPU at known stretches (a bus transaction, a request it
// expects) holds those stretches as windows; writers go in the pauses.
//
//   Holder:  id = WriteGate::addHolder("rs485a");          (setup)
//            WriteGate::open(id);  ...  WriteGate::close(id, pauseMs);
//            pauseMs: this holder's next window starts that long after the
//            close at the earliest (<= 0: no pause); close(id): no
//            forecast. Cheap; any task, not an ISR.
//   Writer on its own task:  WriteSlot slot; if (slot.ok()) { one step }
//   Writer on a shared task (async_tcp, the loop: never waits): only while
//            the gate is free (WritePass pass(now, true)), else it hands the
//            write to FlashWriter (power/FlashWriter.h), never retries.
//   A step is one bounded flash operation: a file of up to ~4 KB, or one
//   sector erase. Longer writes take a slot per step.
//
// The rules (WriteGateCore):
//   - no window ever, or none for idleMs, and none forecast close: free,
//     writes go as without the gate (once a holder has opened windows: one
//     at a time);
//   - in a window: wait;
//   - between windows: ONE writer per pause, the first in the queue, if
//     the earliest forecast window is still >= minPauseMs away (holders
//     without a forecast don't limit it);
//   - a writer that waited maxWaitMs takes any pause with time left; one
//     that waited maxWaitMs + maxWindowMs goes anyway (forced): the write
//     matters more than a delay;
//   - a window open longer than maxWindowMs ends by itself (counted).
// PowerGuard decides first: while it pauses writes nothing goes (a pass or
// a slot is not ok) and the writer keeps its data, as before.
//
// Waiting writers block on a semaphore of their own, given when a window
// ends or a write finishes, and look again under the lock (a window that
// opened meanwhile sends them back to wait, their place kept). No polling.
// Same on every chip.

#include <stdint.h>

#include <atomic>

#include "power/WriteGateCore.h"

namespace ESPRack {

class WriteGate {
 public:
  static void configure(const WriteGateConfig& c);
  static WriteGateConfig config();
  static uint32_t now();  // the gate's clock, ms

  // ---- holders ----
  // At setup. -1 when all WriteGateCore::kMaxHolders are taken. The first
  // holder starts the FlashWriter task (writes will be put off from now on;
  // internal RAM is still to be had at setup).
  static int8_t addHolder(const char* name);
  static void open(int8_t id);
  static void close(int8_t id);                   // no forecast
  static void close(int8_t id, int32_t pauseMs);  // the next window in pauseMs at the earliest

  // ---- writers ----
  // Without waiting (any task): true = write now, then leave(*heldSince).
  // since: when this write was first put off. onlyFree: only while the
  // gate is free (a write of several steps).
  static bool tryEnter(uint32_t since, bool onlyFree = false, uint32_t* heldSince = nullptr);
  static bool tryEnter() { return tryEnter(now()); }
  // tryEnter() would say true now (takes nothing).
  static bool ready(uint32_t since, bool onlyFree = false);
  // Blocking, first come first served: never on async_tcp or the loop
  // task. boundMs: go anyway after this long (0 = the gate's rules);
  // hurry: when it turns true (and nudge() is called), go at once; since:
  // when the writer started to wait for this write (0: now), so maxWaitMs
  // counts across the steps of one write. false only when PowerGuard
  // pauses writes.
  static bool enter(uint32_t boundMs = 0, const std::atomic<bool>* hurry = nullptr, uint32_t since = 0,
                    uint32_t* heldSince = nullptr);
  // The write that went at heldSince is done.
  static void leave(uint32_t heldSince);
  // A writer handed a write on for later (for the counter).
  static void noteDeferred();
  // Wakes every waiting writer to look again (e.g. after setting its hurry flag).
  static void nudge();

  // ---- state, for holders (leave a pause to a waiting writer) and pages ----
  static uint8_t waiting();   // writers blocked in enter() now
  static uint8_t writing();   // writes under way
  static bool inWindow();
  static bool isFree();       // no window open or near: a write of several steps fits
  static WriteGateStats stats();
  static void resetStats();   // e.g. at the start of a bench run
  static uint8_t holders();
  static const char* holderName(int8_t id);
};

// enter() / leave() for a scope (own tasks). again(): leave, then wait for
// the next slot (a write of several steps), with the same wait clock.
class WriteSlot {
 public:
  explicit WriteSlot(uint32_t boundMs = 0, const std::atomic<bool>* hurry = nullptr, uint32_t since = 0)
      : _bound(boundMs), _hurry(hurry), _since(since ? since : WriteGate::now()) {
    take();
  }
  ~WriteSlot() { release(); }
  bool ok() const { return _ok; }
  void release() {
    if (_ok) WriteGate::leave(_heldSince);
    _ok = false;
  }
  bool again() {
    release();
    take();
    return _ok;
  }
  WriteSlot(const WriteSlot&) = delete;
  WriteSlot& operator=(const WriteSlot&) = delete;

 private:
  void take() { _ok = WriteGate::enter(_bound, _hurry, _since, &_heldSince); }
  uint32_t _bound;
  const std::atomic<bool>* _hurry;
  uint32_t _since;
  bool _ok = false;
  uint32_t _heldSince = 0;
};

// tryEnter() / leave() for a scope (shared tasks: never waits).
class WritePass {
 public:
  explicit WritePass(uint32_t since, bool onlyFree = false) : _ok(WriteGate::tryEnter(since, onlyFree, &_at)) {}
  WritePass() : WritePass(WriteGate::now()) {}
  ~WritePass() { release(); }
  bool ok() const { return _ok; }
  void release() {
    if (_ok) WriteGate::leave(_at);
    _ok = false;
  }
  WritePass(const WritePass&) = delete;
  WritePass& operator=(const WritePass&) = delete;

 private:
  uint32_t _at = 0;  // before _ok: set by tryEnter in _ok's initialiser
  bool _ok;
};

}  // namespace ESPRack

#endif  // ESPRACK_WRITE_GATE_H
