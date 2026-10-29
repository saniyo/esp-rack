#include "system/FirmwareUpdate.h"

#include <Arduino.h>

#include <atomic>

namespace ESPRack {
namespace FirmwareUpdate {

namespace {
std::atomic<const char*> s_source{nullptr};  // null: no update
std::atomic<uint32_t> s_lastMs{0};
}  // namespace

void begin(const char* source) {
  s_lastMs = millis();
  s_source = source ? source : "update";
}

bool tryBegin(const char* source) {
  const char* want = source ? source : "update";
  const char* cur = s_source.load();
  if (cur && millis() - s_lastMs.load() < ESPRACK_FW_UPDATE_STALL_MS) return false;  // another writer
  s_lastMs = millis();  // first: whoever sees the new source sees a fresh time
#ifdef ESP8266
  s_source = want;  // one core, callers don't preempt each other
  return true;
#else
  return s_source.compare_exchange_strong(cur, want);  // static storage: RMW is fine
#endif
}

void progress() { s_lastMs = millis(); }

void end() { s_source = nullptr; }

bool active() {
  return s_source.load() != nullptr && millis() - s_lastMs.load() < ESPRACK_FW_UPDATE_STALL_MS;
}

const char* source() {
  const char* s = s_source.load();
  return s && active() ? s : "";
}

}  // namespace FirmwareUpdate
}  // namespace ESPRack
