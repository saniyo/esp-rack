#pragma once
#ifndef ESPRACK_PURE_LIMIT_MONITOR_H
#define ESPRACK_PURE_LIMIT_MONITOR_H

// LimitMonitor — a threshold with hysteresis and dwell times.
//   upper: exceeds when v > limit, clears when v < limit - hyst
//   lower: exceeds when v < limit, clears when v > limit + hyst
// Trips after the exceed condition held continuously for tripMs, clears
// after the clear condition held for clearMs (0 = on the first sample).
// An invalid sample (or NaN) restarts the pending dwell and never changes
// the tripped state.

#include <stdint.h>

namespace ESPRack {

class LimitMonitor {
 public:
  struct Cfg {
    float limit, hyst;
    bool upper;
    uint32_t tripMs, clearMs;
  };
  enum class Ev : uint8_t { None, Tripped, Cleared };

  explicit LimitMonitor(const Cfg& cfg) : _cfg(cfg) {}

  Ev feed(float v, bool valid, uint64_t monoMs);
  bool tripped() const { return _tripped; }
  void reset() {
    _tripped = false;
    _pending = false;
  }

 private:
  Cfg _cfg;
  bool _tripped = false;
  bool _pending = false;
  uint64_t _since = 0;
};

}  // namespace ESPRack

#endif  // ESPRACK_PURE_LIMIT_MONITOR_H
