#pragma once
#ifndef HeapMonitor_h
#define HeapMonitor_h

// HeapMonitor — Phase 4 of docs/plans/memory-fragmentation-master-plan.md.
//
// Continuously samples the system heap and keeps a ring buffer of the
// last hour's readings. Surfaces:
//   * Current free / max-alloc / min-ever-seen
//   * Drift slope (max-alloc change over the last hour)
//   * Sparkline data exposable via REST for a live UI chart
//
// Why this matters for certified uptime: max-alloc is the EARLY
// indicator of TLS handshake failure. mbedtls needs ~32 KB
// contiguous; once max-alloc dips below that we're one handshake
// away from a stuck device. With per-minute sampling + slope
// detection an operator (or upstream fleet monitor) sees the device
// degrade hours before failure instead of in the post-mortem.
//
// Zero heap impact — the entire ring lives in BSS. The tick handler
// makes no allocations.

#include <stdint.h>
#include <stddef.h>

namespace ESPRack {

struct HeapSample {
  uint32_t uptime_s;     // device millis() / 1000 when sampled
  uint32_t free_bytes;
  uint32_t max_alloc;
  uint32_t min_free_ever; // ESP.getMinFreeHeap() — low-water from boot
};

struct HeapSnapshot {
  // Most-recent sample.
  HeapSample latest;
  // First sample we kept (= boot conditions, modulo ring overflow).
  HeapSample boot;
  // Slope of max-alloc over the last hour, signed bytes/min.
  // Negative => fragmentation accumulating.
  int32_t max_alloc_slope_bpm;
  // Number of valid samples in the ring (1..RING_CAPACITY).
  uint8_t sample_count;
};

class HeapMonitor {
 public:
  // Per-minute tick. Cheap; just reads ESP heap APIs and stores into
  // the ring. Called from App::loop on a 60 s throttle.
  static void tick();

  // Snapshot for callers (System tab UI render, /checkin body, etc.).
  // O(N) but N=60 max — fits comfortably in a render path.
  static HeapSnapshot snapshot();

  // Direct access to the ring for JSON sparkline emission. Caller
  // gets newest-first order (idx 0 = latest). Returns true and fills
  // out_sample if the requested index is in range.
  static bool sampleAt(uint8_t newest_first_idx, HeapSample& out_sample);
  static uint8_t sampleCount();

  // One-shot diagnostic line — useful at boot to anchor the "what
  // does FRESH heap look like?" reference. Logged via log_i.
  static void logSnapshot(const char* tag);
};

}  // namespace ESPRack

#endif  // HeapMonitor_h
