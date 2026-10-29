#pragma once
#ifndef ESPRACK_HW_IRS485_PORT_H
#define ESPRACK_HW_IRS485_PORT_H

// IRs485Port ("rs485Port.v1"): an RS485 line the ESP drives itself (DE from
// the ESP, never a transceiver that turns itself around), for protocols with
// hard timing such as Modbus RTU. The port is an external HW instance
// (installExtHw<>); the software on the bus finds it and claims it:
//
//   ESPRack::IRs485Port* port = app->hw()->find<ESPRack::IRs485Port>("rs485a");
//   if (port && port->claim("myBus") && port->begin(cfg)) ...
//   ESPRack::Rs485Frame f;
//   while (port->read(f, 1000)) { if (f.flags & ESPRack::kTxDone) ...; else ... }
//
//   * Receive by frames, not bytes: a frame is the bytes between two
//     silences of t3.5, each byte stamped in the interrupt (esp_timer µs).
//     The owner blocks on the port's queue; nothing polls.
//   * send() is non-blocking: the port waits for t3.5 of silence, drives DE,
//     and puts a kTxDone record in the same queue when DE drops. Records
//     come in time order: a TxDone is before any frame whose first byte came
//     after its tLastUs.
//   * Listen: the transmitter is locked (DE never active); send() fails.
//     With autoDetect the port finds the baud rate, parity and stop bits
//     by CRC validity (parity/framing errors rule a setting out at once).
//   * lastBeginWhy() says why begin() failed. The port's own settings
//     (pins, UART, enabled) belong to the device, not to the owner: when
//     they change, a running port reopens on the new ones with the line in
//     effect, or stops (running() false, no end() needed: the next begin()
//     reads them again). A send the change cut, or one not yet started,
//     comes back as kTxDone | kTxDropped; records already queued stay.
//   * setSignal(): the owner's wake-up for what it did not do itself (new
//     settings applied, a refused begin() that may now succeed), so it waits
//     on its own task notification instead of calling begin() on a timer.
//
// Threads: begin/end/claim/release/setSignal from the loop task or the
// owner's task; read/send from the owner's task only.

#include <stddef.h>
#include <stdint.h>

namespace ESPRack {

enum class PortRole : uint8_t { Off = 0, Down, Up, Listen };
enum class Parity : uint8_t { None = 0, Even, Odd };

struct Rs485Config {
  uint32_t baud = 19200;
  Parity parity = Parity::None;
  uint8_t stopBits = 1;
  uint32_t t15Us = 0;  // 0 = from the baud rate (Modbus: 11-bit chars, fixed above 19200)
  uint32_t t35Us = 0;
  PortRole role = PortRole::Listen;
  bool autoDetect = false;  // listen: find baud/parity/stop bits by CRC validity
};

// Record flags. RX: 0..4 (+ collision); kTxDone marks the end of a send.
enum Rs485RxFlag : uint16_t {
  kRxCrcOk = 1u << 0,
  kRxGapT15 = 1u << 1,     // a pause > t1.5 inside the frame
  kRxOverrun = 1u << 2,    // bytes were lost (FIFO or ring full, or > 256 bytes)
  kRxEcho = 1u << 3,       // our own bytes, read back (a separate /RE pin)
  kRxJunkPrefix = 1u << 4, // 0xFF before a frame whose CRC holds without it
  kTxDone = 1u << 5,       // a send finished: data/len = what went out, tFirst/tLast = DE on/off
  kRxCollision = 1u << 6,  // the echo differed from what was sent
  // With kTxDone: the send did not go out whole. New settings reopened or
  // stopped the port before it started or while it was on the line (DE
  // dropped mid-frame); data/len = the frame, tFirst/tLast = when dropped.
  kTxDropped = 1u << 7,
};

struct Rs485Frame {
  uint8_t data[256];
  uint16_t len;
  uint16_t flags;
  int64_t tFirstUs;  // RX: first byte received; TX: DE on
  int64_t tLastUs;   // RX: last byte received;  TX: DE off
};

struct Rs485Stats {
  uint32_t rxFrames = 0;
  uint32_t txFrames = 0;
  uint32_t rxBytes = 0;
  uint32_t crcErrors = 0;
  uint32_t t15Violations = 0;
  uint32_t t35Violations = 0;  // a frame began less than t3.5 after our send ended
  uint32_t overruns = 0;
  uint32_t echoes = 0;
  uint32_t collisions = 0;
  uint32_t framingErrors = 0;
  uint32_t parityErrors = 0;
  uint32_t junkPrefix = 0;
  uint32_t txBusy = 0;         // send() refused: busy, listen or not running
  uint32_t txDropped = 0;      // sends handed back with kTxDropped
  uint32_t restarts = 0;       // the port reopened or stopped on new settings
  uint8_t autodetect = 0;      // 0 off, 1 searching, 2 found
  // Interrupt -> port task latency: < 50, 100, 200, 500 µs, 1, 2, 5 ms, more.
  uint32_t latencyUs[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};

// Why begin() did not start the port.
enum class Rs485Begin : uint8_t {
  Ok = 0,
  Busy,         // another begin()/end() or the settings held a lock past its bound: retry after a timeout
  Off,          // role Off, or the port is switched off in its settings
  HwNotReady,   // pins not set, or in conflict (the port's page says why)
  StartFailed,  // the UART did not open (memory, the interrupt), or new settings closed it meanwhile
};

// The owner's wake-up: only wake (xTaskNotifyGive, an event bit), never
// block or call the port. Any task may call it, the port's task included.
using Rs485Signal = void (*)(void* ctx);

class IRs485Port {
 public:
  static constexpr const char* kIfaceId = "rs485Port.v1";
  virtual ~IRs485Port() = default;

  // true: the port runs. false: lastBeginWhy() says why.
  virtual bool begin(const Rs485Config& cfg) = 0;
  // Why the last begin() did not start the port (Ok after a success; a port
  // that does not tell says Ok). Busy: retry after a timeout of the owner's
  // own (a lock was held past its bound; one held by another begin()/end()
  // is also signalled when it finishes). Off, HwNotReady, StartFailed:
  // begin() again on the signal (setSignal) - new settings, or settings that
  // came in during this begin(). Without a signal the owner retries on a
  // timeout. Read it from the task that called begin(), right after it.
  virtual Rs485Begin lastBeginWhy() const { return Rs485Begin::Ok; }
  // Stops the port; a send not yet out is dropped without a record.
  virtual void end() = 0;
  // begin() succeeded and the UART is open now: false after end(), and
  // while new settings keep the port closed (the owner is signalled).
  virtual bool running() const = 0;
  // The line settings in effect (after auto-detection in listen).
  virtual Rs485Config config() const = 0;

  virtual bool claim(const char* user) = 0;
  // Also clears the signal (setSignal).
  virtual void release(const char* user) = 0;
  virtual const char* claimedBy() const = 0;

  // fn(ctx) when new settings were applied (running() may have changed:
  // reopened or stopped) or a refused begin() may now succeed. nullptr
  // clears it. false: the port has no signal (retry on a timeout).
  virtual bool setSignal(Rs485Signal fn, void* ctx) {
    (void)fn;
    (void)ctx;
    return false;
  }

  // The next record (a frame, or a kTxDone), waiting up to timeoutMs.
  virtual bool read(Rs485Frame& f, uint32_t timeoutMs) = 0;
  // Queue a frame for sending (at most 256 bytes); false in listen, when
  // not running, or while a send is still under way.
  virtual bool send(const uint8_t* d, size_t n) = 0;

  virtual Rs485Stats stats() const = 0;
};

}  // namespace ESPRack

#endif  // ESPRACK_HW_IRS485_PORT_H
