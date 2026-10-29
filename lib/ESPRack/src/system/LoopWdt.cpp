#include "system/LoopWdt.h"

#include <Arduino.h>  // enableLoopWDT / disableLoopWDT
#include <esp_task_wdt.h>
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
uint32_t s_timeoutMs = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000;

// Held for an esp_task_wdt add/delete (microseconds); a wait this long
// means something is stuck: the call is skipped (logged).
constexpr TickType_t kLockWait = pdMS_TO_TICKS(500);
struct Lock {
  explicit Lock(const char* who) {
    if (!s_mx) s_mx = xSemaphoreCreateMutexStatic(&s_mxBuf);  // first call: early boot, one task
    ok = xSemaphoreTake(s_mx, kLockWait) == pdTRUE;
    if (!ok) log_e("[wdt] %s: lock not had in 500 ms — skipped", who);
  }
  ~Lock() {
    if (ok) xSemaphoreGive(s_mx);
  }
  bool ok;
};
}  // namespace

// The TWDT with a new timeout; the idle tasks sdkconfig subscribes stay as
// they are.
void reconfigure(uint32_t ms) {
  uint32_t idle = 0;
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
  idle |= 1u << 0;
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
  idle |= 1u << 1;
#endif
  const esp_task_wdt_config_t cfg{ms, idle, true};
  if (esp_task_wdt_reconfigure(&cfg) == ESP_OK) {
    s_timeoutMs = ms;
  } else {
    log_w("[wdt] could not set the task watchdog to %u ms", (unsigned)ms);
  }
}

void arm(uint32_t timeoutMs) {
  Lock l("arm");
  if (!l.ok) return;
  if (timeoutMs && timeoutMs != s_timeoutMs) reconfigure(timeoutMs);
  if (s_armed) return;
  s_armed = true;
  if (s_paused == 0) enableLoopWDT();
}

void pause() {
  Lock l("pause");  // skipped: the watchdog stays on (the safe side)
  if (!l.ok) return;
  if (s_paused++ == 0 && s_armed) disableLoopWDT();
}

void resume() {
  Lock l("resume");
  if (!l.ok) return;
  if (s_paused == 0) return;  // unbalanced: ignore
  if (--s_paused == 0 && s_armed) enableLoopWDT();
}

bool armed() { return s_armed; }
uint32_t timeoutMs() { return s_timeoutMs; }
bool paused() { return s_paused > 0; }

}  // namespace LoopWdt
}  // namespace ESPRack
