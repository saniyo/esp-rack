#pragma once
#ifndef ESPRACK_SYSTEM_LOOP_WDT_H
#define ESPRACK_SYSTEM_LOOP_WDT_H

// LoopWdt: the Arduino loop task on the task watchdog
// (TWDT, panic). Arduino feeds it once per loop(); a loop() iteration that
// blocks longer than the timeout resets the chip.
//
// Opt-in: the consumer calls arm(), typically right after app->begin().
// The framework pauses it where the loop legitimately blocks: an ArduinoOTA
// transfer (the OTA module) and the shutdown hooks of a restart (bounded by
// the restart deadline instead, App.h). pause()/resume() nest and may be
// called from any task, and are never skipped: the count is kept under a
// spinlock, and the task that sees it change brings the watchdog in line
// (Settle below), outside the spinlock (esp_task_wdt_add allocates). A task
// that finds a transition under way leaves it to that task, which looks
// again before it returns. App::loop waits half the timeout at most.
//
// Timeout: sdkconfig's CONFIG_ESP_TASK_WDT_TIMEOUT_S (5 s) unless arm() gets
// one, or the build sets ESPRACK_LOOP_WDT_MS. The TWDT is one for all its
// tasks, so the new timeout applies to the others too (async_tcp, the idle
// tasks sdkconfig subscribes). Set it once, at start.

#include <stdint.h>

#ifndef ESPRACK_LOOP_WDT_MS
#define ESPRACK_LOOP_WDT_MS 0  // 0 = keep sdkconfig's timeout
#endif

namespace ESPRack {
namespace LoopWdt {

// The pause count and the watchdog's state, without the lock (the caller
// holds it). Pure, so the host tests run it. A change of armed/paused is
// followed by begin(): +1 enable, -1 disable (the caller does it, no lock
// held, then calls end()), 0 nothing to do or another task is on it. end()
// true: something changed meanwhile, begin() again.
struct Settle {
  bool armed = false;
  int paused = 0;
  bool on = false;     // the watchdog watches the loop task (or is being made to)
  bool busy = false;   // a task is between begin() and end()
  bool dirty = false;  // a change came while busy

  int begin() {
    if (busy) {
      dirty = true;
      return 0;
    }
    const bool want = armed && paused == 0;
    if (want == on) return 0;
    on = want;
    busy = true;
    dirty = false;
    return want ? 1 : -1;
  }
  bool end() {
    busy = false;
    const bool again = dirty;
    dirty = false;
    return again;
  }
};

void arm(uint32_t timeoutMs = ESPRACK_LOOP_WDT_MS);  // 0 = keep the current timeout
uint32_t timeoutMs();                                 // the TWDT timeout now
void pause();
void resume();
bool armed();
bool paused();

}  // namespace LoopWdt
}  // namespace ESPRack

#endif  // ESPRACK_SYSTEM_LOOP_WDT_H
