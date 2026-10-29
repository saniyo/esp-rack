#include "system/LoopWdt.h"

#include <Arduino.h>  // enableLoopWDT / disableLoopWDT
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace ESPRack {
namespace LoopWdt {

namespace {
// The pause count under a spinlock (a few loads/stores); the TWDT calls run
// outside it (see Settle in the header).
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
Settle s_st;
uint32_t s_timeoutMs = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000;

// Brings the watchdog in line with armed/paused. Never waits: a transition
// under way on another task is finished, and looked at again, by that task.
void settle() {
  for (;;) {
    portENTER_CRITICAL(&s_mux);
    const int step = s_st.begin();
    portEXIT_CRITICAL(&s_mux);
    if (step == 0) return;
    if (step > 0) enableLoopWDT();
    else disableLoopWDT();
    portENTER_CRITICAL(&s_mux);
    const bool again = s_st.end();
    portEXIT_CRITICAL(&s_mux);
    if (!again) return;
  }
}
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

// Once, at start (the timeout is not changed under a running pause).
void arm(uint32_t timeoutMs) {
  if (timeoutMs && timeoutMs != s_timeoutMs) reconfigure(timeoutMs);
  portENTER_CRITICAL(&s_mux);
  s_st.armed = true;
  portEXIT_CRITICAL(&s_mux);
  settle();
}

void pause() {
  portENTER_CRITICAL(&s_mux);
  ++s_st.paused;
  portEXIT_CRITICAL(&s_mux);
  settle();
}

void resume() {
  portENTER_CRITICAL(&s_mux);
  const bool balanced = s_st.paused > 0;
  if (balanced) --s_st.paused;
  portEXIT_CRITICAL(&s_mux);
  if (!balanced) {
    log_w("[wdt] resume without a pause: ignored");
    return;
  }
  settle();
}

bool armed() { return s_st.armed; }
uint32_t timeoutMs() { return s_timeoutMs; }
bool paused() { return s_st.paused > 0; }

}  // namespace LoopWdt
}  // namespace ESPRack
