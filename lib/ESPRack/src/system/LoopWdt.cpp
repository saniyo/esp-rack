#include "system/LoopWdt.h"

#include <Arduino.h>  // enableLoopWDT / disableLoopWDT
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace ESPRack {
namespace LoopWdt {

namespace {
// The enable/disable transitions are serialised: a pause on one task and a
// resume on another must not leave the watchdog on while paused.
StaticSemaphore_t s_mxBuf;
SemaphoreHandle_t s_mx = nullptr;
bool s_armed = false;
int s_paused = 0;

struct Lock {
  Lock() {
    if (!s_mx) s_mx = xSemaphoreCreateMutexStatic(&s_mxBuf);  // first call: early boot, one task
    xSemaphoreTake(s_mx, portMAX_DELAY);
  }
  ~Lock() { xSemaphoreGive(s_mx); }
};
}  // namespace

void arm() {
  Lock l;
  if (s_armed) return;
  s_armed = true;
  if (s_paused == 0) enableLoopWDT();
}

void pause() {
  Lock l;
  if (s_paused++ == 0 && s_armed) disableLoopWDT();
}

void resume() {
  Lock l;
  if (s_paused == 0) return;  // unbalanced: ignore
  if (--s_paused == 0 && s_armed) enableLoopWDT();
}

bool armed() { return s_armed; }
bool paused() { return s_paused > 0; }

}  // namespace LoopWdt
}  // namespace ESPRack
