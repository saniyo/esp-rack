#pragma once
#ifndef ESPRACK_HW_RS485_FRAMER_H
#define ESPRACK_HW_RS485_FRAMER_H

// Rs485Framer: the RS485 / Modbus RTU frame state machine, pure (no RTOS,
// no hardware) so it runs in host tests on an injected clock. A port feeds
// it each received byte with the time the interrupt stamped (about the end
// of its stop bit) and asks it to close the open frame once the line has
// been silent for t3.5.
//
//   framer.reset(rs485::timingFor(cfg));
//   if (framer.onByte(b, tUs, errFlags, out)) emit(out);   // a frame closed first
//   if (framer.poll(nowUs, out)) emit(out);                // t3.5 of silence
//
// Silence before a byte = its time - the previous byte's time - one char.
// A pause > t1.5 inside a frame flags kRxGapT15; >= t3.5 starts a new one.

#include <stddef.h>
#include <stdint.h>

#include "hw/IRs485Port.h"

namespace ESPRack {
namespace rs485 {

struct Timing {
  uint32_t charUs = 0;  // one character on the line (start + 8 + parity + stop)
  uint32_t t15Us = 0;
  uint32_t t35Us = 0;
};

// The timing of a line: Modbus uses 1.5 / 3.5 characters of 11 bits, and
// fixed 750 / 1750 µs above 19200 baud. Non-zero t15Us / t35Us in the
// config win.
Timing timingFor(const Rs485Config& cfg);

// Modbus RTU CRC (0xA001, init 0xFFFF); on the wire low byte first.
uint16_t crc16(const uint8_t* d, size_t n);
// The last two bytes are the CRC of the rest.
bool crcOk(const uint8_t* d, size_t n);

class Framer {
 public:
  void reset(const Timing& t);
  // A byte at tUs; errFlags: kRxOverrun when bytes were lost before it.
  // true: the open frame closed before this byte (in `out`).
  bool onByte(uint8_t b, int64_t tUs, uint16_t errFlags, Rs485Frame& out);
  // true: the open frame closed (silence >= t3.5 by nowUs), in `out`.
  bool poll(int64_t nowUs, Rs485Frame& out);
  bool open() const { return open_; }
  // When poll() would close the open frame (0: none open).
  int64_t deadlineUs() const { return open_ ? last_ + t_.t35Us : 0; }
  int64_t lastUs() const { return last_; }
  const Timing& timing() const { return t_; }

 private:
  void close(Rs485Frame& out);
  Timing t_;
  Rs485Frame cur_{};
  bool open_ = false;
  int64_t last_ = 0;
};

}  // namespace rs485
}  // namespace ESPRack

#endif  // ESPRACK_HW_RS485_FRAMER_H
