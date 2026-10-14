#include "power/PowerGuard.h"

#include <atomic>

namespace ESPRack {

namespace {
std::atomic<uint8_t> s_state{static_cast<uint8_t>(SupplyState::Unknown)};
std::atomic<int32_t> s_mv{-1};
std::atomic<bool> s_latched{false};
PowerGuard::Listener s_listeners[PowerGuard::kMaxListeners];
}  // namespace

void PowerGuard::setSupply(SupplyState s, int32_t millivolts) {
  s_mv = millivolts;
  const uint8_t prev = s_state.exchange(static_cast<uint8_t>(s));
  if (prev == static_cast<uint8_t>(s)) return;
  for (auto& l : s_listeners) {
    if (l) l(s);
  }
}

SupplyState PowerGuard::supply() { return static_cast<SupplyState>(s_state.load()); }
int32_t PowerGuard::millivolts() { return s_mv.load(); }

void PowerGuard::latchWritesOffUntilRestart() { s_latched = true; }
bool PowerGuard::latched() { return s_latched.load(); }

bool PowerGuard::writesAllowed() { return !s_latched.load() && supply() != SupplyState::PowerFail; }

const char* PowerGuard::name(SupplyState s) {
  switch (s) {
    case SupplyState::Unknown: return "unknown";
    case SupplyState::AcOk: return "AC ok";
    case SupplyState::UsbOnly: return "USB only";
    case SupplyState::PowerFail: return "power fail";
    case SupplyState::Fault: return "fault";
  }
  return "?";
}

int8_t PowerGuard::subscribe(Listener l) {
  for (int8_t i = 0; i < static_cast<int8_t>(kMaxListeners); ++i) {
    if (!s_listeners[i]) {
      s_listeners[i] = std::move(l);
      return i;
    }
  }
  return -1;
}

void PowerGuard::resetForTest() {
  s_state = static_cast<uint8_t>(SupplyState::Unknown);
  s_mv = -1;
  s_latched = false;
  for (auto& l : s_listeners) l = nullptr;
}

}  // namespace ESPRack
