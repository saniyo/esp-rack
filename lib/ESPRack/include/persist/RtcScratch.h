#pragma once
#ifndef ESPRACK_PERSIST_RTC_SCRATCH_H
#define ESPRACK_PERSIST_RTC_SCRATCH_H

// RtcScratch<T>: a CRC-checked copy of T in RTC memory (LP RAM
// on the C6). It survives a soft reset (software, panic, watchdog, deep
// sleep), not a power cycle, brownout or reset pin. Two slots: commit()
// writes the older one, so a reset halfway through keeps the newer. RAM
// only, no flash: commit as often as needed.
//
//   ESPRACK_RTC_SCRATCH(Ckpt, s_ckpt);   // file scope
//   Ckpt c{};
//   if (s_ckpt.get(c)) restore(c);       // boot: an image survived
//   s_ckpt.commit(state);                // every step
//
// Use it from one task.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>
#include <type_traits>

#include "persist/Crc32.h"

namespace ESPRack {

// Did the last reset keep RTC memory? Target: esp_reset_reason(), once.
bool rtcScratchTrusted();

template <typename T>
struct RtcScratchSlot {
  uint32_t magic;
  uint32_t seq;
  uint32_t len;
  uint32_t crc;  // over seq, len and the payload
  T payload;
};

template <typename T>
struct RtcScratchSlots {
  RtcScratchSlot<T> s[2];
};

template <typename T>
class RtcScratch {
  static_assert(std::is_trivially_copyable<T>::value, "RtcScratch<T>: T must be trivially copyable");

 public:
  static constexpr uint32_t kMagic = 0x31435452u;  // bytes 'R','T','C','1'

  explicit RtcScratch(RtcScratchSlots<T>& slots) : _s(slots) {}

  // A verified image exists: from before a soft reset, or committed since.
  bool valid() {
    init();
    return newest() != nullptr;
  }
  // The newest image into out; false (out untouched) when there is none.
  bool get(T& out) {
    init();
    const RtcScratchSlot<T>* n = newest();
    if (!n) return false;
    memcpy(&out, &n->payload, sizeof(T));
    return true;
  }
  // The newest image's sequence number, +1 per commit; 0 = none.
  uint32_t seq() {
    init();
    const RtcScratchSlot<T>* n = newest();
    return n ? n->seq : 0;
  }
  void commit(const T& v) {
    init();
    const RtcScratchSlot<T>* n = newest();
    RtcScratchSlot<T>& t = (n == &_s.s[0]) ? _s.s[1] : _s.s[0];
    uint32_t seq = n ? n->seq + 1 : 1;
    if (seq == 0) seq = 1;
    t.magic = 0;  // a reset from here on leaves this slot invalid
    barrier();
    memcpy(&t.payload, &v, sizeof(T));
    t.seq = seq;
    t.len = sizeof(T);
    t.crc = crcOf(t);
    barrier();
    t.magic = kMagic;
  }
  void invalidate() {
    _s.s[0].magic = 0;
    _s.s[1].magic = 0;
    _init = true;
  }

 private:
  static void barrier() { std::atomic_signal_fence(std::memory_order_seq_cst); }
  static uint32_t crcOf(const RtcScratchSlot<T>& x) {
    uint32_t c = crc32Ieee(&x.seq, sizeof(x.seq));
    c = crc32Ieee(&x.len, sizeof(x.len), c);
    return crc32Ieee(&x.payload, sizeof(T), c);
  }
  static bool ok(const RtcScratchSlot<T>& x) { return x.magic == kMagic && x.len == sizeof(T) && x.crc == crcOf(x); }
  const RtcScratchSlot<T>* newest() const {
    const bool a = ok(_s.s[0]);
    const bool b = ok(_s.s[1]);
    if (a && b) return static_cast<int32_t>(_s.s[1].seq - _s.s[0].seq) > 0 ? &_s.s[1] : &_s.s[0];
    return a ? &_s.s[0] : (b ? &_s.s[1] : nullptr);
  }
  // After a reset that didn't keep RTC memory the slots hold garbage: wipe
  // them before anything reads them.
  void init() {
    if (_init) return;
    _init = true;
    if (!rtcScratchTrusted()) invalidate();
  }

  RtcScratchSlots<T>& _s;
  bool _init = false;
};

}  // namespace ESPRack

#if defined(ESP_PLATFORM)
#include <esp_attr.h>
// Defines `name`: a RtcScratch<T> over two slots in RTC no-init memory.
// File scope only.
#define ESPRACK_RTC_SCRATCH(T, name)                                  \
  RTC_NOINIT_ATTR static ::ESPRack::RtcScratchSlots<T> name##_slots; \
  static ::ESPRack::RtcScratch<T> name(name##_slots)
#endif

#endif  // ESPRACK_PERSIST_RTC_SCRATCH_H
