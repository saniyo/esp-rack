#include "power/FlashWriter.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

#include "power/PowerGuard.h"

namespace ESPRack {

namespace {

struct JobSlot {
  const char* name = nullptr;
  FlashWriter::Job job;
  std::atomic<bool> kicked{false};
  // the writer task only
  bool onPower = false;
  bool timed = false;
  uint32_t dueAt = 0;
};

JobSlot s_jobs[FlashWriter::kMaxJobs];
std::atomic<uint8_t> s_count{0};
std::atomic<TaskHandle_t> s_task{nullptr};
std::atomic<bool> s_creating{false};
std::atomic<bool> s_powerKick{false};  // PowerGuard allows writes again: run the jobs it held
bool s_listening = false;              // attach() got a PowerGuard listener
constexpr uint32_t kPowerRetryMs = 1000;  // without one: a job PowerGuard held looks again this often

struct FlushSlot {
  FlashWriter::RestartFlush fn = nullptr;
  FlashWriter::RestartFlushMs ms = nullptr;
  void* ctx = nullptr;
};
FlushSlot s_flush[FlashWriter::kMaxRestartFlush];
std::atomic<uint8_t> s_flushCount{0};

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void run(void*) {
  // Known before the first pass: a kick that finds no task yet set its flag
  // first, and this pass sees it.
  s_task = xTaskGetCurrentTaskHandle();
  for (;;) {
    // Run first: kicks made before the task existed are waiting.
    const bool power = s_powerKick.exchange(false);
    uint32_t now = nowMs();
    const uint8_t n = s_count.load();
    for (uint8_t i = 0; i < n; ++i) {
      JobSlot& j = s_jobs[i];
      bool go = j.kicked.exchange(false);
      if (j.onPower && power) go = true;
      if (j.timed && int32_t(now - j.dueAt) >= 0) go = true;
      if (!go || !j.job) continue;
      j.onPower = false;
      j.timed = false;
      const uint32_t r = j.job();
      now = nowMs();
      if (r == FlashWriter::kOnPower && s_listening) {
        j.onPower = true;
      } else if (r == FlashWriter::kOnPower) {
        j.timed = true;
        j.dueAt = now + kPowerRetryMs;
      } else if (r != FlashWriter::kDone) {
        j.timed = true;
        j.dueAt = now + r;
      }
    }
    // Then wait for a kick, or the earliest retry.
    TickType_t wait = portMAX_DELAY;
    now = nowMs();
    for (uint8_t i = 0; i < n; ++i) {
      const JobSlot& j = s_jobs[i];
      if (!j.timed) continue;
      const int32_t left = int32_t(j.dueAt - now);
      const TickType_t t = left > 0 ? pdMS_TO_TICKS(uint32_t(left)) + 1 : 0;
      if (wait == portMAX_DELAY || t < wait) wait = t;
    }
    if (wait) (void)ulTaskNotifyTake(pdTRUE, wait);
  }
}

void ensureTask() {
  if (s_task.load()) return;
  if (s_creating.exchange(true)) return;  // another kick is starting it; it runs every kicked job first
  if (xTaskCreatePinnedToCore(run, "flashw", ESPRACK_FLASH_WRITER_STACK, nullptr, ESPRACK_FLASH_WRITER_PRIO,
                              nullptr, tskNO_AFFINITY) != pdPASS) {
    ESP_LOGE("flashw", "the writer task did not start: deferred writes wait");
    s_creating = false;  // the next kick tries again
  }
}

}  // namespace

int8_t FlashWriter::add(const char* name, Job job) {
  const uint8_t i = s_count.load();
  if (i >= kMaxJobs) return -1;
  s_jobs[i].name = name ? name : "?";
  s_jobs[i].job = std::move(job);
  s_count = static_cast<uint8_t>(i + 1);
  return static_cast<int8_t>(i);
}

void FlashWriter::kick(int8_t id) {
  if (id < 0 || id >= static_cast<int8_t>(s_count.load())) return;
  s_jobs[id].kicked = true;
  ensureTask();
  if (TaskHandle_t t = s_task.load()) xTaskNotifyGive(t);
}

void FlashWriter::attach() {
  if (s_listening) return;
  s_listening = PowerGuard::subscribe([](SupplyState) {
                  if (!PowerGuard::writesAllowed()) return;
                  s_powerKick = true;
                  if (TaskHandle_t t = s_task.load()) xTaskNotifyGive(t);
                }) >= 0;
  if (!s_listening) ESP_LOGW("flashw", "no PowerGuard listener left: held writes look again every %u ms",
                             (unsigned)kPowerRetryMs);
}

void FlashWriter::start() { ensureTask(); }

void FlashWriter::retry() {
  if (s_task.load() || s_creating.load()) return;  // running (or starting): kicks wake it
  static uint32_t last = 0;
  const uint32_t now = nowMs();
  if (now - last < 1000) return;
  bool any = false;
  for (uint8_t i = 0, n = s_count.load(); i < n; ++i) any = any || s_jobs[i].kicked.load();
  if (!any) return;
  last = now;
  ensureTask();  // it runs every job still kicked
}

bool FlashWriter::addRestartFlush(RestartFlush fn, RestartFlushMs ms, void* ctx) {
  const uint8_t i = s_flushCount.load();
  if (!fn || i >= kMaxRestartFlush) return false;
  s_flush[i].fn = fn;
  s_flush[i].ms = ms;
  s_flush[i].ctx = ctx;
  s_flushCount = static_cast<uint8_t>(i + 1);
  return true;
}

void FlashWriter::restartFlush(uint32_t boundMs) {
  for (uint8_t i = 0, n = s_flushCount.load(); i < n; ++i) s_flush[i].fn(s_flush[i].ctx, boundMs);
}

uint32_t FlashWriter::restartFlushMs(uint32_t boundMs) {
  uint32_t ms = 0;
  for (uint8_t i = 0, n = s_flushCount.load(); i < n; ++i) {
    if (s_flush[i].ms) ms += s_flush[i].ms(s_flush[i].ctx, boundMs);
  }
  return ms;
}

uint8_t FlashWriter::jobs() { return s_count.load(); }

const char* FlashWriter::name(int8_t id) {
  return (id >= 0 && id < static_cast<int8_t>(s_count.load())) ? s_jobs[id].name : "";
}

bool FlashWriter::waiting(int8_t id) {
  if (id < 0 || id >= static_cast<int8_t>(s_count.load())) return false;
  const JobSlot& j = s_jobs[id];
  return j.kicked.load() || j.onPower || j.timed;
}

bool FlashWriter::started() { return s_task.load() != nullptr; }

}  // namespace ESPRack
