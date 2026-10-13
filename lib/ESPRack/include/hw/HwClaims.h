#pragma once
#ifndef ESPRACK_HW_CLAIMS_H
#define ESPRACK_HW_CLAIMS_H

// The board's pin facts and the conflict rules between hardware claims
// (LIB-FEAT-001). Arduino-free so the rules are unit-tested on a PC; the
// target builds the BoardInfo from sdkconfig (hw/BoardProfile) and checks
// the tables against the SoC headers at compile time.

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "hw/HwResource.h"

namespace ESPRack {

enum class HwChip : uint8_t { Esp32, Esp32S3, Esp32C3, Esp32C6, Unknown };

enum class PinLevel : uint8_t {
  Reserved,  // validate() fails: flash, PSRAM, USB/UART0 console, reserveGpio()
  Caution    // ok with a warning: strapping pins
};

struct PinNote {
  int8_t gpio;
  PinLevel level;
  const char* reason;  // static literal
};

struct BoardFlags {
  bool psram = false;               // PSRAM enabled (its CS pin is taken)
  bool octalPsramOrFlash = false;   // octal PSRAM or flash: the extra data pins are taken
  bool usbInUse = false;            // USB-Serial-JTAG / CDC console on the USB pins
  bool uart0Console = true;         // UART0 is the console (Serial); else only the ROM boot log uses it
};

struct BoardInfo {
  HwChip chip = HwChip::Unknown;
  const char* chipName = "?";
  uint8_t gpioCount = 0;
  uint64_t validMask = 0;   // GPIOs that exist
  uint64_t outputMask = 0;  // GPIOs that can drive
  uint64_t adc2Mask = 0;    // ADC2 pins (unreliable while Wi-Fi is active)
  // Peripheral budgets
  uint8_t uarts = 0, i2cBuses = 0, spiBuses = 0;
  uint8_t rmtTx = 0, rmtRx = 0, rmtTotal = 0;  // rmtTotal: shared TX/RX pool (classic ESP32)
  uint8_t ledc = 0, pcnt = 0;
  bool uart0Console = false;  // UART0 is the console: not claimable

  static constexpr uint8_t kMaxNotes = 32;
  PinNote notes[kMaxNotes];
  uint8_t noteCount = 0;

  bool addNote(int8_t gpio, PinLevel level, const char* reason);
  // The strongest note for a GPIO (Reserved over Caution), or nullptr.
  const PinNote* note(int gpio) const;
  bool exists(int gpio) const { return gpio >= 0 && gpio < 64 && ((validMask >> gpio) & 1u); }
  bool canOutput(int gpio) const { return gpio >= 0 && gpio < 64 && ((outputMask >> gpio) & 1u); }
  bool isAdc2(int gpio) const { return gpio >= 0 && gpio < 64 && ((adc2Mask >> gpio) & 1u); }
};

// The pin tables of a chip, as reserved/caution notes for these flags.
BoardInfo boardInfoFor(HwChip chip, const BoardFlags& flags);

struct HwClaim {
  const char* owner;  // instance id (stable storage, the module's)
  HwResource res;
};

class HwClaims {
 public:
  explicit HwClaims(const BoardInfo& board) : _b(board) {}

  const BoardInfo& board() const { return _b; }
  BoardInfo& board() { return _b; }

  // Would `owner` holding exactly `d` be valid against the board and every
  // OTHER owner's claims? (owner's own current claims are ignored)
  HwCheck check(const char* owner, const HwDescriptor& d) const;

  void set(const char* owner, const HwDescriptor& d);  // replaces owner's claims
  void remove(const char* owner);

  size_t size() const { return _claims.size(); }
  const HwClaim& at(size_t i) const { return _claims[i]; }
  // Claims of one kind held by owners other than `owner` (any index).
  uint8_t countOthers(HwKind kind, const char* owner) const;

 private:
  BoardInfo _b;
  std::vector<HwClaim> _claims;
};

}  // namespace ESPRack

#endif  // ESPRACK_HW_CLAIMS_H
