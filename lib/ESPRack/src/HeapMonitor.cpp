#include "HeapMonitor.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

// 60 samples × 1 min cadence = one hour ring. Tunable here if we
// ever want to push the visible window longer (cost: 16 B per slot).
constexpr uint8_t RING_CAPACITY = 60;

ESPRack::HeapSample g_ring[RING_CAPACITY] = {};
// Number of valid samples (saturates at RING_CAPACITY).
uint8_t g_count = 0;
// Index where the NEXT sample will be written. Wraps mod RING_CAPACITY.
uint8_t g_write_idx = 0;

// Returns the array index of the sample that's `newest_first_idx`
// positions back from the most recent. 0 → latest, 1 → previous, ...
inline uint8_t ringPos(uint8_t newest_first_idx) {
  // `g_write_idx` points at where the NEXT write goes — the most
  // recent live sample is at write_idx - 1.
  uint16_t pos = (uint16_t)g_write_idx + RING_CAPACITY - 1 - newest_first_idx;
  return (uint8_t)(pos % RING_CAPACITY);
}

}  // namespace

namespace ESPRack {

void HeapMonitor::tick() {
  HeapSample& slot = g_ring[g_write_idx];
  slot.uptime_s      = (uint32_t)(millis() / 1000);
  slot.free_bytes    = ESP.getFreeHeap();
  slot.max_alloc     = ESP.getMaxAllocHeap();
  slot.min_free_ever = ESP.getMinFreeHeap();
  g_write_idx = (uint8_t)((g_write_idx + 1) % RING_CAPACITY);
  if (g_count < RING_CAPACITY) g_count++;
}

uint8_t HeapMonitor::sampleCount() { return g_count; }

bool HeapMonitor::sampleAt(uint8_t newest_first_idx, HeapSample& out) {
  if (newest_first_idx >= g_count) return false;
  out = g_ring[ringPos(newest_first_idx)];
  return true;
}

HeapSnapshot HeapMonitor::snapshot() {
  HeapSnapshot s = {};
  s.sample_count = g_count;
  if (g_count == 0) {
    // Pre-first-tick — synthesise from live readings so callers see
    // SOMETHING instead of zeros.
    s.latest.uptime_s      = (uint32_t)(millis() / 1000);
    s.latest.free_bytes    = ESP.getFreeHeap();
    s.latest.max_alloc     = ESP.getMaxAllocHeap();
    s.latest.min_free_ever = ESP.getMinFreeHeap();
    s.boot = s.latest;
    return s;
  }
  // Newest = ring at write_idx - 1; oldest = (write_idx - g_count).
  // Both modulo RING_CAPACITY.
  s.latest = g_ring[ringPos(0)];
  s.boot   = g_ring[ringPos(g_count - 1)];
  // Slope over the captured window — bytes per minute.
  if (g_count >= 2) {
    int64_t delta_bytes = (int64_t)s.latest.max_alloc - (int64_t)s.boot.max_alloc;
    uint32_t delta_s = s.latest.uptime_s - s.boot.uptime_s;
    if (delta_s > 0) {
      // Convert to bytes/min — saturate to int32_t for the snapshot field.
      int64_t bpm = (delta_bytes * 60) / (int64_t)delta_s;
      if (bpm > INT32_MAX) bpm = INT32_MAX;
      if (bpm < INT32_MIN) bpm = INT32_MIN;
      s.max_alloc_slope_bpm = (int32_t)bpm;
    }
  }
  return s;
}

void HeapMonitor::logSnapshot(const char* tag) {
  // Read ESP heap APIs LIVE here — ring samples land only on the
  // 60-second tick cadence, so a snapshot() right after an event
  // (WG up, TLS handshake, etc.) used to log the previous tick's
  // stale boot-time numbers and disagree with whatever the web UI
  // showed via getFreeHeap(). Mixing live event-time numbers with
  // ring-derived boot_max + slope gives both freshness AND trend.
  HeapSnapshot s = snapshot();
  uint32_t live_free      = ESP.getFreeHeap();
  uint32_t live_max_alloc = ESP.getMaxAllocHeap();
  uint32_t live_min_free  = ESP.getMinFreeHeap();
  log_i("[heap.mon] %s: free=%u max_alloc=%u min_free_ever=%u "
        "boot_max=%u slope=%dB/min samples=%u",
        tag ? tag : "?",
        (unsigned)live_free,
        (unsigned)live_max_alloc,
        (unsigned)live_min_free,
        (unsigned)s.boot.max_alloc,
        (int)s.max_alloc_slope_bpm,
        (unsigned)s.sample_count);
}

void HeapMonitor::dumpTasks(const char* tag) {
  // Snapshot every FreeRTOS task. Allocates a temporary array of
  // TaskStatus_t — one element per current task, ~52 bytes each.
  // At ~20 tasks we transiently use ~1 KB heap; that's a one-shot
  // call from boot/diagnostic paths, not the hot loop, so OK.
  const UBaseType_t n = uxTaskGetNumberOfTasks();
  if (n == 0) return;
  TaskStatus_t* tasks = static_cast<TaskStatus_t*>(
      pvPortMalloc(n * sizeof(TaskStatus_t)));
  if (!tasks) {
    log_w("[task.dump] alloc failed (n=%u)", (unsigned)n);
    return;
  }
  const UBaseType_t got = uxTaskGetSystemState(tasks, n, nullptr);
  log_i("[task.dump] %s — %u tasks (stack_free_min is uxTaskGetStackHighWaterMark in BYTES)",
        tag ? tag : "?", (unsigned)got);
  for (UBaseType_t i = 0; i < got; i++) {
    // Convert StackType_t words to bytes. On ESP32 (Xtensa) and C3
    // (RISC-V) StackType_t is uint8_t for the return value of
    // uxTaskGetStackHighWaterMark — it's already in *bytes* per the
    // ESP-IDF port. (FreeRTOS upstream returns words; ESP-IDF added
    // a bytes shim. Either way the value is "smallest free room
    // ever seen".)
    log_i("[task.dump]   %-16s prio=%2u state=%u stack_free_min=%u",
          tasks[i].pcTaskName,
          (unsigned)tasks[i].uxCurrentPriority,
          (unsigned)tasks[i].eCurrentState,
          (unsigned)tasks[i].usStackHighWaterMark);
  }
  vPortFree(tasks);
}

}  // namespace ESPRack
