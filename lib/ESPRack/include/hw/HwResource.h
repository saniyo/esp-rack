#pragma once
#ifndef ESPRACK_HW_RESOURCE_H
#define ESPRACK_HW_RESOURCE_H

// Hardware resources an external HW module instance uses (LIB-FEAT-001).
// Plain data, Arduino-free (host-tested in test-host/).
//
// A resource is a kind + an index: a GPIO number, a UART port, an I2C bus
// or address, an RMT/LEDC/PCNT channel. index -1 = not configured (the
// instance is NotConfigured), -2 = "any channel" for peripherals that
// IDF 5 / Arduino 3 allocate at run time (RMT, LEDC, PCNT) — counted
// against the chip's budget instead of a number.

#include <stddef.h>
#include <stdint.h>

namespace ESPRack {

enum class HwKind : uint8_t {
  Gpio,      // index = GPIO
  Uart,      // index = UART port
  I2cBus,    // index = I2C bus. A bus instance (hw/II2cBus.h) holds it and SDA/SCL exclusively; a driver
             // that opens the bus itself: kHwShared, SDA/SCL as Gpio, kHwShared, bus = the bus, role "sda"/"scl"
  I2cAddr,   // index = 7-bit address, bus = I2C bus (-1 = bus 0); a device on a bus instance: i2cAddrClaim()
  SpiBus,    // index = SPI host (0 = the first general-purpose one); pins as for I2C ("sck"...)
  SpiCs,     // index = CS GPIO, bus = SPI host
  RmtTx,     // index = channel or kHwAnyChannel
  RmtRx,
  Adc,       // index = GPIO (ADC2 pins get a Wi-Fi warning)
  Ledc,      // index = channel or kHwAnyChannel
  PcntUnit,  // index = unit or kHwAnyChannel
  Other      // informational only; role says what it is
};

// Several instances may hold it: a bus, and a bus's pins when they agree on
// the bus number and the role (SDA of I2C0 is shared only as SDA of I2C0).
constexpr uint8_t kHwShared = 0x01;
constexpr uint8_t kHwOptional = 0x02;  // the instance works without it (-1 doesn't make it NotConfigured)
constexpr uint8_t kHwOutput = 0x04;    // drives the pin (refused on input-only pins)

constexpr int16_t kHwNotConfigured = -1;
constexpr int16_t kHwAnyChannel = -2;

struct HwResource {
  HwKind kind;
  int16_t index;
  int8_t bus;          // I2cAddr / SpiCs; -1 otherwise
  uint8_t flags;
  const char* role;    // static literal: "tx", "rx", "sda", "relay", ...
};

struct HwDescriptor {
  static constexpr uint8_t kMax = 12;
  HwResource res[kMax];
  uint8_t count = 0;
  bool disabled = false;  // the instance is switched off: holds nothing, state Disabled

  // false when full
  bool add(HwKind kind, int16_t index, const char* role, uint8_t flags = 0, int8_t bus = -1) {
    if (count >= kMax) return false;
    res[count++] = HwResource{kind, index, bus, flags, role};
    return true;
  }
  void clear() {
    count = 0;
    disabled = false;
  }
  // The first required resource left at -1 (no kHwOptional): the instance
  // is NotConfigured for want of it. nullptr when there's none.
  const HwResource* missing() const {
    for (uint8_t i = 0; i < count; ++i) {
      if (res[i].index == kHwNotConfigured && !(res[i].flags & kHwOptional)) return &res[i];
    }
    return nullptr;
  }
};

// Kinds whose index is a GPIO number.
inline bool hwIsPin(HwKind k) { return k == HwKind::Gpio || k == HwKind::Adc || k == HwKind::SpiCs; }

enum class HwState : uint8_t { Ok, NotConfigured, Conflict, Disabled };

// A runtime note an instance attaches to itself (SES-LIB-025): the registry's
// Ok only says the resources are free — not that the UART answered.
enum class HwNoteLevel : uint8_t { Info, Warn, Fault };

struct HwCheck {
  bool ok = true;
  bool warning = false;  // ok, but see reason (strapping pin, ADC2 with Wi-Fi)
  char reason[96] = {0};
};

const char* hwKindName(HwKind k);
const char* hwStateName(HwState s);
// "GPIO17", "UART1", "I2C0", "I2C0 0x68", "RMT TX any" — into buf
const char* hwResourceText(const HwResource& r, char* buf, size_t len);

}  // namespace ESPRack

#endif  // ESPRACK_HW_RESOURCE_H
