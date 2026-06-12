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
//
// DEBUG-GATED (FT_HEAP_MONITOR, default 0). The fragmentation hunt this
// was built for is closed, so shipping builds compile the ring + all
// logging out to no-ops; `snapshot()` alone stays live (it backs an
// operator-facing System-tab row, not debug). Re-enable with
// `-D FT_HEAP_MONITOR=1` in features.ini for the next heap investigation.

#include <stdint.h>
#include <stddef.h>
#include <Features.h>

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
#if FT_ENABLED(FT_HEAP_MONITOR)
  // Per-minute tick. Cheap; just reads ESP heap APIs and stores into
  // the ring. Called from App::loop on a 60 s throttle.
  static void tick();

  // Direct access to the ring for JSON sparkline emission. Caller
  // gets newest-first order (idx 0 = latest). Returns true and fills
  // out_sample if the requested index is in range.
  static bool sampleAt(uint8_t newest_first_idx, HeapSample& out_sample);
  static uint8_t sampleCount();

  // One-shot diagnostic line — useful at boot to anchor the "what
  // does FRESH heap look like?" reference. Logged via log_i.
  static void logSnapshot(const char* tag);

  // Dump per-FreeRTOS-task stack usage. Uses uxTaskGetSystemState +
  // uxTaskGetStackHighWaterMark to report, for every task in the
  // system, its name + priority + minimum free stack ever (in
  // bytes). Lets us spot oversized task stacks — Mothership/Telegram
  // are sized 8 KB / 6 KB but the real high-water on a clean device
  // is usually a fraction of that. Trim to actual usage × 1.5 to
  // free BSS for the heap pool. Needs
  // CONFIG_FREERTOS_USE_TRACE_FACILITY (on by default for ESP-IDF
  // Arduino).
  static void dumpTasks(const char* tag);
#else
  // ── FT_HEAP_MONITOR=0: instrumentation compiled out ─────────────────
  // Every entry point is a no-op so the ~20 logSnapshot/tick call sites
  // across App / WireGuard / Mothership keep compiling untouched, the
  // ring leaves BSS, and the optimizer drops the unused tag strings.
  static inline void tick() {}
  static inline bool sampleAt(uint8_t, HeapSample&) { return false; }
  static inline uint8_t sampleCount() { return 0; }
  static inline void logSnapshot(const char*) {}
  static inline void dumpTasks(const char*) {}
#endif

  // Snapshot for callers (System tab UI render, /checkin body, etc.).
  // Live in BOTH modes — it backs an operator-facing row, not debug.
  // FT_HEAP_MONITOR=1 → newest ring sample + hour slope (O(N), N≤60);
  // =0 → a synthesised live reading (sample_count=0, slope=0).
  static HeapSnapshot snapshot();
};

}  // namespace ESPRack

#endif  // HeapMonitor_h
