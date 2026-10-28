#pragma once
#ifndef ESPRACK_WRITE_GATE_CORE_H
#define ESPRACK_WRITE_GATE_CORE_H

// WriteGateCore: the rules of WriteGate (power/WriteGate.h) on a clock the
// caller passes in (ms, wrapping), without locks or tasks. The caller
// serialises the calls. A time older than one already seen counts as that
// one (callers read the clock before they serialise). Arduino-free
// (host-tested).

#include <stdint.h>

#ifndef ESPRACK_WRITE_MIN_PAUSE_MS
#define ESPRACK_WRITE_MIN_PAUSE_MS 60  // less left before a forecast window: no writer
#endif
#ifndef ESPRACK_WRITE_MAX_WINDOW_MS
#define ESPRACK_WRITE_MAX_WINDOW_MS 2000  // a window open longer ends by itself
#endif
#ifndef ESPRACK_WRITE_MAX_WAIT_MS
#define ESPRACK_WRITE_MAX_WAIT_MS 10000  // a writer that waited this long takes any pause
#endif
#ifndef ESPRACK_WRITE_IDLE_MS
#define ESPRACK_WRITE_IDLE_MS 4000  // no window this long (none forecast): free (> a bus master's retry time)
#endif

namespace ESPRack {

struct WriteGateConfig {
  uint32_t minPauseMs = ESPRACK_WRITE_MIN_PAUSE_MS;
  uint32_t maxWindowMs = ESPRACK_WRITE_MAX_WINDOW_MS;
  uint32_t maxWaitMs = ESPRACK_WRITE_MAX_WAIT_MS;
  uint32_t idleMs = ESPRACK_WRITE_IDLE_MS;
};

struct WriteGateStats {
  uint32_t windows = 0;   // windows opened
  uint32_t expired = 0;   // ... that outlived maxWindowMs
  uint32_t grants = 0;    // writes let through
  uint32_t inPause = 0;   // ... into a pause between windows (one per pause)
  uint32_t forced = 0;    // ... after a long wait: into a short pause, a window, or a bound that ran out
  uint32_t deferred = 0;  // writes their writer handed on for later (WriteGate::noteDeferred)
  uint32_t refused = 0;   // asks PowerGuard turned down
  uint32_t longestWaitMs = 0;
  uint32_t longestHoldMs = 0;      // the longest write let through between windows (pause, forced)
  uint32_t longestFreeHoldMs = 0;  // ... while the gate was free (boot, quiet times)
  uint32_t overlaps = 0;           // writes that ran into a window
  uint32_t longestOverlapMs = 0;   // the longest stretch of a window and a write together
};

class WriteGateCore {
 public:
  static constexpr uint8_t kMaxHolders = 4;
  static constexpr uint8_t kMaxQueue = 16;

  enum class Verdict : uint8_t { Go, Wait, Refused };
  struct Decision {
    Verdict verdict;
    uint32_t retryMs;  // Wait: ask again within this long (a window ending may come sooner)
    uint32_t at = 0;   // Go: when; leave() takes it back
  };
  // A blocking writer's place in the queue: writers between windows go first
  // come, first served. 0 = none (a writer that doesn't wait: it never jumps
  // the queue).
  using Ticket = uint16_t;

  void configure(const WriteGateConfig& c) { _cfg = c; }
  const WriteGateConfig& config() const { return _cfg; }

  // -1 when all kMaxHolders are taken.
  int8_t addHolder(const char* name);
  // Opening an open window does nothing (its limit counts from the first open).
  void open(int8_t h, uint32_t now);
  // No forecast: this holder doesn't limit the pause.
  void close(int8_t h, uint32_t now);
  // pauseMs: this holder's next window starts that long after now at the
  // earliest; <= 0: no pause (no writer goes before its next window, or
  // before the forecast stops counting: idleMs after it without a window).
  void close(int8_t h, uint32_t now, int32_t pauseMs);

  Ticket join(uint32_t now);  // a blocking writer starts to wait (0 when the queue is full)
  void quit(Ticket t);        // ... and stops (it went, was refused or gave up)

  // A writer waiting since `since` asks to write now. Go: it writes, then
  // calls leave(). onlyFree: only while the gate is free (a write of
  // several steps or tens of KB: no pause fits it).
  Decision enter(uint32_t now, uint32_t since, bool onlyFree = false, Ticket t = 0);
  // The same question without taking anything: would enter() say Go?
  bool wouldGo(uint32_t now, uint32_t since, bool onlyFree = false, Ticket t = 0);
  // Go now whatever the windows and the queue (a writer's bounded wait ran
  // out); Refused only by PowerGuard.
  Decision force(uint32_t now, uint32_t since);
  // The write that went at `heldSince` (Decision::at) is done.
  void leave(uint32_t now, uint32_t heldSince);
  void noteDeferred() { ++_stats.deferred; }
  void resetStats() { _stats = WriteGateStats{}; }

  bool windowOpen(uint32_t now);  // ends windows past maxWindowMs first
  // No window open, none for idleMs (or ever), and none forecast within
  // maxWindowMs (a forecast idleMs overdue no longer counts): a write of
  // several steps fits.
  bool isFree(uint32_t now);
  uint8_t openWindows(uint32_t now);
  uint8_t writing() const { return _writing; }
  uint8_t queued() const { return _qLen; }
  uint8_t holders() const { return _count; }
  const char* holderName(int8_t h) const;
  bool holderOpen(int8_t h) const;
  const WriteGateStats& stats() const { return _stats; }

 private:
  struct Holder {
    const char* name = nullptr;
    bool open = false;
    uint32_t openedAt = 0;
    bool forecast = false;  // the last close said when its next window comes
    uint32_t nextAt = 0;    // ... then: no window before this
  };

  uint32_t clamp(uint32_t now);  // never older than a time already seen
  Decision decide(uint32_t now, uint32_t since, bool onlyFree, Ticket t, bool commit);
  Decision grant(uint32_t now, uint32_t since, bool forced, bool inPause, bool inWindow, bool commit);
  void closeAt(int8_t h, uint32_t now, bool forecast, uint32_t nextAt);
  void endWindow(uint32_t at);
  void startOverlap(uint32_t now);
  void endOverlap(uint32_t now);
  uint32_t untilExpiry(uint32_t now) const;  // of the window that ends first
  bool headOf(Ticket t) const;               // t may go first (no ticket: only with an empty queue)
  // The pause left before the earliest forecast window, ms (<= 0: none
  // left); any = false when no holder gave a forecast that still counts.
  int32_t pauseLeft(uint32_t now, bool& any) const;
  bool idle(uint32_t now) const { return !_used || now - _lastEnd >= _cfg.idleMs; }
  static uint32_t lesser(uint32_t a, uint32_t b) { return a < b ? a : b; }

  WriteGateConfig _cfg;
  Holder _h[kMaxHolders];
  uint8_t _count = 0;
  bool _used = false;        // a window was ever opened
  bool _seen = false;        // _lastNow is set
  uint32_t _lastNow = 0;
  uint32_t _lastEnd = 0;     // when the last window ended (closed or expired)
  bool _pause = false;       // between windows: none open since _lastEnd
  bool _pauseTaken = false;  // a writer went in this pause
  uint8_t _writing = 0;
  bool _gatedOn = false;     // the write under way went between windows (not free)
  uint32_t _gatedAt = 0;
  bool _overlapOn = false;   // a window and a write together
  uint32_t _overlapAt = 0;
  Ticket _queue[kMaxQueue] = {};
  uint8_t _qLen = 0;
  Ticket _nextTicket = 1;
  WriteGateStats _stats;
};

}  // namespace ESPRack

#endif  // ESPRACK_WRITE_GATE_CORE_H
