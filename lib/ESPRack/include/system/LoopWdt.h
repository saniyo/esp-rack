#pragma once
#ifndef ESPRACK_SYSTEM_LOOP_WDT_H
#define ESPRACK_SYSTEM_LOOP_WDT_H

// LoopWdt (SES-LIB-014): the Arduino loop task on the task watchdog
// (sdkconfig: CONFIG_ESP_TASK_WDT_TIMEOUT_S = 5 s, panic). Arduino feeds it
// once per loop(); a loop() iteration that blocks longer resets the chip.
//
// Opt-in: the consumer calls arm(), typically right after app->begin().
// The framework pauses it where the loop legitimately blocks: an ArduinoOTA
// transfer (the OTA module) and the shutdown hooks of a restart.
// pause()/resume() nest and may be called from any task.

namespace ESPRack {
namespace LoopWdt {

void arm();
void pause();
void resume();
bool armed();
bool paused();

}  // namespace LoopWdt
}  // namespace ESPRack

#endif  // ESPRACK_SYSTEM_LOOP_WDT_H
