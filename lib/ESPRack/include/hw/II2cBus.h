#pragma once
#ifndef ESPRACK_HW_II2C_BUS_H
#define ESPRACK_HW_II2C_BUS_H

// II2cBus — one I2C bus shared by the devices on it (SES-LIB-034).
//
// The BUS is an external HW instance of its own ("i2c-bus": one I2C
// controller and its SDA/SCL pins). It claims HwKind::I2cBus (index = the
// controller number) and both pins exclusively, starts the controller and
// answers queryInterface(II2cBus::kIfaceId). Nobody else opens Wire on that
// controller.
//
// A DEVICE on the bus (an RTC, a FRAM, an ADC ... each from its own driver
// module) is an external HW instance that claims only its address, no pins.
// The bus instance's id is one of its settings:
//
//   void describeHw(HwDescriptor& d) const override {
//     ESPRack::i2cDeviceClaim(hw(), _cfg.busId, 0x68, "rtc", d);
//   }
//   // onBegin, or after a settings change:
//   _bus = hw()->find<ESPRack::II2cBus>(_cfg.busId);
//
// Two devices conflict only on the same address on the same controller.
// A device without a bus (no such instance, the id not set) is
// NotConfigured.
//
// The bus comes before its devices in install order: its onBegin then
// starts the controller before theirs run, and the boot pass gives it the
// controller and pins first. Builder orders by requires_, then priority
// (not by the order of the install calls), so the bus module's priority is
// below every device driver's. A device still copes with a bus that isn't
// ready (yet, or restarting after a settings change): its calls return
// NotReady and it tries again on a later pass.
//
// A driver of the old kind, one that opens Wire itself and claims
// HwKind::I2cBus + SDA/SCL as kHwShared, can't share a controller with a
// bus instance: whichever of the two comes second is Conflict ("I2C0 used
// by i2c0").
//
// Threading: write/read/writeRead/probe from any task. Each call is one
// transaction, serialised by the bus lock and bounded in time (about 20 ms:
// the lock wait plus the transfer); it never blocks for long. A device that
// needs several transactions with nothing in between (a FRAM's address,
// then its page) holds the lock across them with an I2cBusGuard. The lock
// is recursive per task, so the transactions inside the guard take it
// again without waiting.
//
// Same contract on every chip; a chip with two controllers is two bus
// instances. The interface carries no Arduino or driver types (host-tested).

#include <stddef.h>
#include <stdint.h>

#include "hw/HwResource.h"

namespace ESPRack {

class HwRegistry;

enum class I2cResult : uint8_t {
  Ok,
  Nack,      // the device didn't acknowledge its address or a byte
  Timeout,   // the transfer didn't complete in time (stuck bus, clock stretching)
  Busy,      // another task held the bus lock longer than the call may wait
  NotReady,  // the bus isn't started (not configured, Conflict, Disabled)
  TooLong,   // more than II2cBus::kMaxXfer bytes in one direction
  Error      // anything else (bad arguments, a driver error)
};

// "ok", "nack", "timeout", "busy", "not-ready", "too-long", "error" — for the
// UI and logs.
inline const char* i2cResultName(I2cResult r) {
  switch (r) {
    case I2cResult::Ok: return "ok";
    case I2cResult::Nack: return "nack";
    case I2cResult::Timeout: return "timeout";
    case I2cResult::Busy: return "busy";
    case I2cResult::NotReady: return "not-ready";
    case I2cResult::TooLong: return "too-long";
    case I2cResult::Error: return "error";
  }
  return "?";
}

// Counters since the bus was installed (not reset when it restarts).
struct I2cBusStats {
  uint32_t xfers = 0;       // transactions that got the bus (probes included)
  uint32_t nacks = 0;       // Nack results (a probe that finds nothing isn't one)
  uint32_t timeouts = 0;    // Timeout results
  uint32_t busyWaits = 0;   // calls that found the lock held by another task (Busy included)
  uint32_t recoveries = 0;  // stuck-bus recoveries (SCL pulses, controller reset)
  uint32_t errors = 0;      // Error results
};

class II2cBus {
 public:
  static constexpr const char* kIfaceId = "i2cBus.v1";
  // Bytes one transaction moves in one direction at most.
  static constexpr size_t kMaxXfer = 128;

  virtual ~II2cBus() = default;

  // The controller number (0 = I2C0), fixed by the factory: valid before
  // the bus starts, even when it never does. Devices claim their address on
  // it.
  virtual int controller() const = 0;
  // The controller is started on its pins.
  virtual bool ready() const = 0;

  // One transaction each (7-bit `addr`), any task. NotReady when the bus
  // isn't started, TooLong past kMaxXfer, Busy when the lock isn't free in
  // time; never blocks longer than about 20 ms.
  virtual I2cResult write(uint8_t addr, const uint8_t* d, size_t n) = 0;
  virtual I2cResult read(uint8_t addr, uint8_t* out, size_t n) = 0;
  // Write `w`, then read `rn` bytes into `r` after a repeated start (a
  // register read) — no other transaction gets in between.
  virtual I2cResult writeRead(uint8_t addr, const uint8_t* w, size_t wn, uint8_t* r, size_t rn) = 0;
  // An address-only transaction: true when a device acknowledges `addr`.
  virtual bool probe(uint8_t addr) = 0;

  // Hold the bus across several transactions (use I2cBusGuard). Waits up to
  // waitMs; false when it isn't free by then. Recursive per task: every
  // successful lock() needs its unlock(), from the same task.
  virtual bool lock(uint32_t waitMs) = 0;
  virtual void unlock() = 0;

  // Counters (a copy).
  virtual I2cBusStats stats() const = 0;
};

// Holds the bus lock for a scope — several transactions with nothing from
// another task in between:
//
//   ESPRack::I2cBusGuard g(_bus, 50);
//   if (!g.ok()) return;  // busy: try again later
//   _bus->write(0x50, addr, 2);
//   _bus->read(0x50, page, sizeof(page));
//
// ok(): the lock was taken (false for a null bus). The destructor unlocks
// only what it locked.
class I2cBusGuard {
 public:
  I2cBusGuard(II2cBus* bus, uint32_t waitMs) : _bus(bus), _ok(bus != nullptr && bus->lock(waitMs)) {}
  ~I2cBusGuard() {
    if (_ok) _bus->unlock();
  }
  I2cBusGuard(const I2cBusGuard&) = delete;
  I2cBusGuard& operator=(const I2cBusGuard&) = delete;

  bool ok() const { return _ok; }

 private:
  II2cBus* _bus;
  bool _ok;
};

// A device's address claim on `bus`, for describeHw(): HwKind::I2cAddr,
// index = addr, bus = bus->controller(), role = label (a static literal).
// With no bus the address is "not set" (-1): the instance is NotConfigured,
// or merely runs without it when `flags` has kHwOptional. false when `d` is
// full.
inline bool i2cAddrClaim(const II2cBus* bus, uint8_t addr, const char* label, HwDescriptor& d,
                         uint8_t flags = 0) {
  if (!bus) return d.add(HwKind::I2cAddr, kHwNotConfigured, label, flags);
  return d.add(HwKind::I2cAddr, addr, label, flags, static_cast<int8_t>(bus->controller()));
}

// The same, with the bus looked up by its instance id
// (hw->find<II2cBus>(busId)); a null hw, a null or empty id, or no such bus
// instance is "no bus". find() takes no registry lock, so this is safe in
// describeHw(). Defined with the registry (target builds only).
bool i2cDeviceClaim(const HwRegistry* hw, const char* busId, uint8_t addr, const char* label, HwDescriptor& d,
                    uint8_t flags = 0);

}  // namespace ESPRack

#endif  // ESPRACK_HW_II2C_BUS_H
