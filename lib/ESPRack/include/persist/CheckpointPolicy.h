#pragma once
#ifndef ESPRACK_PERSIST_CHECKPOINT_POLICY_H
#define ESPRACK_PERSIST_CHECKPOINT_POLICY_H

// CheckpointPolicy (SES-LIB-008): when a running value (a counter, an energy
// total) is worth a flash write. Never sooner than minIntervalMs after the
// last checkpoint; then as soon as it moved by minDelta, and at the latest
// after maxIntervalOnMs (load on) or maxIntervalOffMs (off). Arduino-free.

#include <stdint.h>

namespace ESPRack {

struct CheckpointPolicy {
  uint32_t minIntervalMs = 60000;        // 1 min
  uint32_t maxIntervalOnMs = 900000;     // 15 min
  uint32_t maxIntervalOffMs = 21600000;  // 6 h
  int64_t minDelta = 20;

  // lastMs: the last checkpoint (millis(), wraps); delta: the value now minus
  // the value then; on: the load is on.
  bool due(uint32_t nowMs, uint32_t lastMs, bool on, int64_t delta) const {
    const uint32_t elapsed = nowMs - lastMs;
    if (elapsed < minIntervalMs) return false;
    if ((delta < 0 ? -delta : delta) >= minDelta) return true;
    return elapsed >= (on ? maxIntervalOnMs : maxIntervalOffMs);
  }
};

}  // namespace ESPRack

#endif  // ESPRACK_PERSIST_CHECKPOINT_POLICY_H
