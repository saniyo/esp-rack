#pragma once
#ifndef ESPRACK_HW_ICAN_PORT_H
#define ESPRACK_HW_ICAN_PORT_H

// ICanPort — one CAN bus interface, as seen by the software that uses it.
//
// Only the CAN *interface* is hardware: an external HW instance that claims
// the pins ("can-twai" for the on-chip TWAI controller, an SPI controller
// later). It answers queryInterface(ICanPort::kIfaceId).
//
// The software on the bus (a battery bus, an inverter link, a protocol) is a
// plain module installed with install<M>(portId). It finds its port in
// onBegin and wraps it in its own library's transport:
//
//   ESPRack::ICanPort* port = app->hw()->find<ESPRack::ICanPort>(portId);
//   if (port && port->claim("jkBus") && port->begin(250000)) ...
//
// One port has one software user at a time: claim(user) takes it, a second
// claim by someone else fails, and claimedBy() shows who holds it (the port's
// page shows it too). The user calls end() and release(user) when it stops,
// e.g. before it restarts with new settings.
//
// Threading: begin/end/claim/release from loopTask. write/read from the
// task of the user that holds the claim (one task per port).
// Same contract on every chip; a chip with two controllers is two ports.

#include <Arduino.h>

namespace ESPRack {

// One classic CAN frame (no CAN FD).
struct CanFrame {
  uint32_t id = 0;     // 11-bit (ext false) or 29-bit (ext true) identifier
  bool ext = false;    // extended (29-bit) frame
  uint8_t len = 0;     // data length 0..8
  uint8_t data[8] = {};
};

// Counters since the port was installed (not reset by begin/end).
struct CanPortStats {
  uint32_t txFrames = 0;   // frames handed to the controller
  uint32_t rxFrames = 0;   // frames received
  uint32_t txErrors = 0;   // write() failures (queue full, bus off, timeout)
  uint32_t rxOverruns = 0; // frames lost because the receive queue was full
  uint32_t busOff = 0;     // times the controller went bus-off
};

class ICanPort {
 public:
  static constexpr const char* kIfaceId = "canPort.v1";

  virtual ~ICanPort() = default;

  // Start the controller at `bitrate` bit/s (e.g. 250000, 500000). false when
  // the bitrate isn't supported, the port isn't configured or its pins are in
  // conflict. Calling it again with a new bitrate restarts the controller.
  virtual bool begin(uint32_t bitrate) = 0;
  // Stop the controller and let the bus go. Safe when not started.
  virtual void end() = 0;
  // Started and not bus-off.
  virtual bool running() const = 0;

  // Queue one frame for sending. false when not running, len > 8 or the
  // transmit queue is full. Never blocks.
  virtual bool write(const CanFrame& f) = 0;
  // Wait up to timeoutMs for one received frame (0 = don't wait). true and
  // `f` filled when a frame came.
  virtual bool read(CanFrame& f, uint32_t timeoutMs) = 0;

  // Counters (a copy); busOff counts how often the controller dropped off
  // the bus. The port recovers by itself while it is started.
  virtual CanPortStats stats() const = 0;

  // Take the port for `user` (a module id, a static string). true when it
  // was free or `user` already holds it; false when someone else does.
  virtual bool claim(const char* user) = 0;
  // Give the port back. Only the holder can release it; others are ignored.
  virtual void release(const char* user) = 0;
  // The holder's id, or nullptr when free.
  virtual const char* claimedBy() const = 0;
};

}  // namespace ESPRack

#endif  // ESPRACK_HW_ICAN_PORT_H
