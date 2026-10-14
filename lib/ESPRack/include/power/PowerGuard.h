#pragma once
#ifndef ESPRACK_POWER_GUARD_H
#define ESPRACK_POWER_GUARD_H

// PowerGuard (SES-LIB-008): the device's supply state and whether flash
// writes are allowed. The consumer sets the state from its own sensing
// (setSupply). Nobody setting it leaves Unknown, and writes stay allowed:
// a device without supply sensing works as before.
//
// Writes stop in PowerFail, and for good after latchWritesOffUntilRestart()
// (brown-in after a power failure is unstable). The state keeps showing the
// real supply. NvsRecord, ConfigManager and the manifest cache ask
// writesAllowed() before they write.
//
// Arduino-free (host-tested). Any task.

#include <stdint.h>

#include <functional>

namespace ESPRack {

// The one supply-state enum of the system (modules use this one).
enum class SupplyState : uint8_t { Unknown, AcOk, UsbOnly, PowerFail, Fault };

class PowerGuard {
 public:
  // millivolts: the measured supply, -1 = unknown. Listeners run on a change
  // of state, on the calling task.
  static void setSupply(SupplyState s, int32_t millivolts = -1);
  static SupplyState supply();
  static int32_t millivolts();

  // No flash writes until the next restart, whatever the supply does next.
  static void latchWritesOffUntilRestart();
  static bool latched();

  // !latched() && supply() != PowerFail. Unknown, AcOk, UsbOnly and Fault
  // allow writes.
  static bool writesAllowed();

  static const char* name(SupplyState s);  // "AC ok", "power fail", ...

  using Listener = std::function<void(SupplyState)>;
  static constexpr uint8_t kMaxListeners = 4;
  // At setup (loop task). -1 when full.
  static int8_t subscribe(Listener l);

  // Host tests: back to Unknown, not latched, no listeners.
  static void resetForTest();
};

}  // namespace ESPRack

#endif  // ESPRACK_POWER_GUARD_H
