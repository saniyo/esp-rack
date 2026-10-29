#pragma once
#ifndef ESPRACK_SYSTEM_LOOP_WAIT_H
#define ESPRACK_SYSTEM_LOOP_WAIT_H

// LoopWait: how long App::loop may block before it runs again (HGW-LIB-003).
// The loop runs its own work and every module's onLoop(), then waits for the
// nearest deadline (Module::loopWaitMs, the core's own) or a wakeLoop()
// (App.h), whichever comes first. Pure: the arithmetic, host-tested.
//
//   ESPRACK_LOOP_DEFAULT_WAIT_MS  Module::loopWaitMs() unless overridden: a
//                                 module not converted yet (a consumer's own
//                                 too) still runs every 10 ms, only without a
//                                 busy loop.
//   ESPRACK_LOOP_IDLE_WAIT_MS     a module with no timer of its own (its
//                                 onLoop only reacts to flags other tasks
//                                 set, and they call wakeLoop()).
//   ESPRACK_LOOP_MAX_WAIT_MS      bounds every wait: what nothing wakes is
//                                 seen within it (a slow cadence, a clock
//                                 edge, code beside app->loop() in loop()).

#include <stdint.h>

#ifndef ESPRACK_LOOP_DEFAULT_WAIT_MS
#define ESPRACK_LOOP_DEFAULT_WAIT_MS 10
#endif
#ifndef ESPRACK_LOOP_MAX_WAIT_MS
#define ESPRACK_LOOP_MAX_WAIT_MS 1000
#endif
#ifndef ESPRACK_LOOP_IDLE_WAIT_MS
#define ESPRACK_LOOP_IDLE_WAIT_MS ESPRACK_LOOP_MAX_WAIT_MS
#endif

namespace ESPRack {
namespace LoopWait {

// ms from nowMs until sinceMs + periodMs (a millis() timer), 0 when due.
// Wrap-safe like `now - since >= period`.
inline uint32_t dueIn(uint32_t nowMs, uint32_t sinceMs, uint32_t periodMs) {
  const uint32_t elapsed = nowMs - sinceMs;
  return elapsed >= periodMs ? 0 : periodMs - elapsed;
}

// The loop's wait: the nearest deadline, bounded by maxMs, and by half the
// loop watchdog's timeout while it is on (wdtMs 0 = off), so a quiet loop
// is fed in time.
inline uint32_t bound(uint32_t nearestMs, uint32_t maxMs, uint32_t wdtMs) {
  uint32_t w = nearestMs < maxMs ? nearestMs : maxMs;
  if (wdtMs && w > wdtMs / 2) w = wdtMs / 2;
  return w;
}

// ms -> RTOS ticks, rounded up: a wait of 1..9 ms at 100 Hz is one tick,
// never 0 (a 0 tick wait would spin). 0 stays 0.
inline uint32_t ticksFor(uint32_t ms, uint32_t tickHz) {
  if (!ms) return 0;
  const uint64_t t = (static_cast<uint64_t>(ms) * tickHz + 999) / 1000;
  return t ? (t > 0xFFFFFFFEull ? 0xFFFFFFFEu : static_cast<uint32_t>(t)) : 1;
}

}  // namespace LoopWait
}  // namespace ESPRack

#endif  // ESPRACK_SYSTEM_LOOP_WAIT_H
