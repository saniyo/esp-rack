#include "hw/Rs485Framer.h"

#include <string.h>

namespace ESPRack {
namespace rs485 {

Timing timingFor(const Rs485Config& cfg) {
  Timing t;
  const uint32_t baud = cfg.baud ? cfg.baud : 9600;
  const uint32_t bits = 1 + 8 + (cfg.parity == Parity::None ? 0 : 1) + (cfg.stopBits >= 2 ? 2 : 1);
  t.charUs = uint32_t((uint64_t(bits) * 1000000u + baud - 1) / baud);
  if (baud > 19200) {
    t.t15Us = 750;
    t.t35Us = 1750;
  } else {
    const uint32_t c11 = uint32_t((uint64_t(11) * 1000000u + baud - 1) / baud);  // Modbus counts 11 bits
    t.t15Us = c11 * 3 / 2;
    t.t35Us = c11 * 7 / 2;
  }
  if (cfg.t15Us) t.t15Us = cfg.t15Us;
  if (cfg.t35Us) t.t35Us = cfg.t35Us;
  return t;
}

uint16_t crc16(const uint8_t* d, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; ++i) {
    c ^= d[i];
    for (int k = 0; k < 8; ++k) c = (c & 1) ? uint16_t((c >> 1) ^ 0xA001) : uint16_t(c >> 1);
  }
  return c;
}

bool crcOk(const uint8_t* d, size_t n) {
  if (n < 3) return false;
  const uint16_t c = crc16(d, n - 2);
  return d[n - 2] == (c & 0xFF) && d[n - 1] == (c >> 8);
}

void Framer::reset(const Timing& t) {
  t_ = t;
  open_ = false;
  last_ = 0;
  cur_.len = 0;
  cur_.flags = 0;
}

void Framer::close(Rs485Frame& out) {
  uint16_t flags = cur_.flags;
  const size_t n = cur_.len;
  if (crcOk(cur_.data, n)) {
    flags |= kRxCrcOk;
  } else {
    // 0xFF before a frame (a line settling): the CRC holds without it.
    size_t k = 0;
    while (k < n && cur_.data[k] == 0xFF) ++k;
    if (k > 0 && crcOk(cur_.data + k, n - k)) flags |= kRxJunkPrefix;
  }
  out = cur_;
  out.flags = flags;
  open_ = false;
  cur_.len = 0;
  cur_.flags = 0;
}

bool Framer::onByte(uint8_t b, int64_t tUs, uint16_t errFlags, Rs485Frame& out) {
  bool closed = false;
  if (open_) {
    const int64_t silence = tUs - last_ - int64_t(t_.charUs);
    if (silence >= int64_t(t_.t35Us)) {
      close(out);
      closed = true;
    } else if (silence > int64_t(t_.t15Us)) {
      cur_.flags |= kRxGapT15;
    }
  }
  if (!open_) {
    open_ = true;
    cur_.len = 0;
    cur_.flags = 0;
    cur_.tFirstUs = tUs;
  }
  cur_.flags |= (errFlags & kRxOverrun);
  if (cur_.len < sizeof(cur_.data)) {
    cur_.data[cur_.len++] = b;
  } else {
    cur_.flags |= kRxOverrun;  // longer than any RTU frame: the rest is dropped
  }
  cur_.tLastUs = tUs;
  last_ = tUs;
  return closed;
}

bool Framer::poll(int64_t nowUs, Rs485Frame& out) {
  if (!open_ || nowUs - last_ < int64_t(t_.t35Us)) return false;
  close(out);
  return true;
}

}  // namespace rs485
}  // namespace ESPRack
