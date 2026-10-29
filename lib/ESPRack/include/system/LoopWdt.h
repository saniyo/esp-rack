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
// called from any task. Their lock is bounded (500 ms): a call that doesn't
// get it is skipped and logged. App::loop waits half the timeout at most.
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

void arm(uint32_t timeoutMs = ESPRACK_LOOP_WDT_MS);  // 0 = keep the current timeout
uint32_t timeoutMs();                                 // the TWDT timeout now
void pause();
void resume();
bool armed();
bool paused();

}  // namespace LoopWdt
}  // namespace ESPRack

#endif  // ESPRACK_SYSTEM_LOOP_WDT_H
