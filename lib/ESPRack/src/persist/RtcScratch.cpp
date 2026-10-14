#include "persist/RtcScratch.h"

#include <esp_system.h>

namespace ESPRack {

bool rtcScratchTrusted() {
  static const bool trusted = [] {
    switch (esp_reset_reason()) {
      case ESP_RST_SW:
      case ESP_RST_PANIC:
      case ESP_RST_INT_WDT:
      case ESP_RST_TASK_WDT:
      case ESP_RST_WDT:
      case ESP_RST_DEEPSLEEP:
        return true;
      default:
        return false;  // power-on, brownout, reset pin, USB, ...
    }
  }();
  return trusted;
}

}  // namespace ESPRack
