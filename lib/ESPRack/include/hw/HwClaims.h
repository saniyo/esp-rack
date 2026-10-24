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
  const char* owner;  // instance id (stable storage, the module's); a reason for reserved ones
  HwResource res;
  // the owner gave it up in its settings but hasn't confirmed
  // yet that the hardware is let go (HwClaims::released). Still blocks others.
  bool releasing = false;
  uint32_t releasingSince = 0;  // ms on the caller's clock
};

// Releasing this long without released() shows as a warning: the module
// probably never says so. The claims stay, since only the module knows the
// hardware is let go.
constexpr uint32_t kHwReleasingWarnMs = 10000;

// One instance's columns in System -> Hardware. Pure, so
// the wording is unit-tested.
struct HwInstStatus {
  HwState state = HwState::NotConfigured;
  const char* reason = "";      // the registry's: conflict, strapping-pin warning
  const char* note = "";        // the instance's runtime note
  HwNoteLevel noteLevel = HwNoteLevel::Info;
  size_t releasing = 0;         // claims still being released
  uint32_t releasingAgeMs = 0;  // the oldest one's age
};
// state: "Ok", "Ok · fault", "Conflict · warn" ...
// note:  reason · runtime note · "releasing 2" (or "releasing 2 for 14 s,
//        no released()" past kHwReleasingWarnMs, which also adds "· warn")
void hwStatusText(const HwInstStatus& s, char* state, size_t stateLen, char* note, size_t noteLen);
// "Ok", "Not configured", "Conflict", "Disabled"
const char* hwStateLabel(HwState s);
// Cuts an incomplete UTF-8 character off the end of s, as a truncating copy
// of a non-ASCII note (Cyrillic, say) leaves it.
void hwUtf8Trim(char* s);

class HwClaims {
 public:
  explicit HwClaims(const BoardInfo& board) : _b(board) {}

  const BoardInfo& board() const { return _b; }
  BoardInfo& board() { return _b; }

  // Would `owner` holding exactly `d` be valid against the board and every
  // OTHER owner's claims? (owner's own current claims are ignored)
  HwCheck check(const char* owner, const HwDescriptor& d) const;

  void set(const char* owner, const HwDescriptor& d);  // replaces owner's claims outright
  void remove(const char* owner);

  // replace(): like set(), but the owner's claims that `d` no
  // longer holds become `releasing` (they keep blocking others) instead of
  // vanishing. releaseAll(): every active claim of the owner becomes
  // releasing. released(): drops the owner's releasing claims — the owner
  // has let go of that hardware.
  // `applied`: the owner set the hardware up under its active claims (it was
  // Ok). If not, there is nothing to let go and they're simply dropped;
  // waiting for released() from an owner that never opened anything would
  // block that hardware for good. `nowMs` stamps new releasing claims.
  void replace(const char* owner, const HwDescriptor& d, bool applied, uint32_t nowMs);
  void releaseAll(const char* owner, bool applied, uint32_t nowMs);
  void released(const char* owner);
  size_t releasingCount(const char* owner) const;
  // Age of the owner's oldest releasing claim (0 = none).
  uint32_t releasingAgeMs(const char* owner, uint32_t nowMs) const;

  // a non-GPIO resource nobody may claim (a UART, an RMT
  // channel ... the consumer or a framework module uses itself). GPIOs are
  // reserved through the board notes (BoardInfo::addNote). `reason` is a
  // static literal. false for pin kinds.
  bool reserve(const HwResource& r, const char* reason);
  size_t reservedCount() const { return _reserved.size(); }
  const HwClaim& reservedAt(size_t i) const { return _reserved[i]; }

  size_t size() const { return _claims.size(); }
  const HwClaim& at(size_t i) const { return _claims[i]; }
  // Claims of one kind held by owners other than `owner` (any index).
  uint8_t countOthers(HwKind kind, const char* owner) const;

 private:
  BoardInfo _b;
  std::vector<HwClaim> _claims;
  std::vector<HwClaim> _reserved;  // owner = reason
};

}  // namespace ESPRack

#endif  // ESPRACK_HW_CLAIMS_H
