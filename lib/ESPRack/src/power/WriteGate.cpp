#include "power/WriteGate.h"

#include "power/FlashWriter.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace ESPRack {

namespace {

constexpr uint8_t kWaiters = 8;  // writers that can wait at once on a semaphore of their own

// One spinlock for the core and the waiter table: every section is a few
// comparisons (holders call open/close from high-priority tasks). The clock
// is read inside it, so the core sees time in the order of the calls.
// FreeRTOS calls (giving the semaphores) stay outside it.
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
WriteGateCore s_core;
bool s_used[kWaiters] = {};
std::atomic<uint8_t> s_waiting{0};

struct Sems {
  StaticSemaphore_t buf[kWaiters + 1];
  SemaphoreHandle_t sem[kWaiters + 1];  // the last one: writers beyond kWaiters share it
  Sems() {
    for (uint8_t i = 0; i <= kWaiters; ++i) sem[i] = xSemaphoreCreateBinaryStatic(&buf[i]);
  }
};
Sems& sems() {
  static Sems s;  // created on first use (thread-safe static)
  return s;
}

struct Lock {
  Lock() { portENTER_CRITICAL(&s_mux); }
  ~Lock() { portEXIT_CRITICAL(&s_mux); }
};

uint32_t clockMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void wakeAll() {
  Sems& s = sems();
  SemaphoreHandle_t give[kWaiters + 1];
  uint8_t n = 0;
  {
    Lock l;
    for (uint8_t i = 0; i < kWaiters; ++i) {
      if (s_used[i]) give[n++] = s.sem[i];
    }
  }
  if (s_waiting.load() > n) give[n++] = s.sem[kWaiters];
  for (uint8_t i = 0; i < n; ++i) xSemaphoreGive(give[i]);
}

}  // namespace

uint32_t WriteGate::now() { return clockMs(); }

void WriteGate::configure(const WriteGateConfig& c) {
  {
    Lock l;
    s_core.configure(c);
  }
  wakeAll();
}

WriteGateConfig WriteGate::config() {
  Lock l;
  return s_core.config();
}

int8_t WriteGate::addHolder(const char* name) {
  int8_t id;
  {
    Lock l;
    id = s_core.addHolder(name);
  }
  if (id >= 0) FlashWriter::start();  // outside the lock: it creates a task
  return id;
}

void WriteGate::open(int8_t id) {
  Lock l;
  s_core.open(id, clockMs());
}

void WriteGate::close(int8_t id) {
  bool pause;
  {
    Lock l;
    const uint32_t t = clockMs();
    s_core.close(id, t);
    pause = !s_core.windowOpen(t);
  }
  if (pause && s_waiting.load()) wakeAll();
}

void WriteGate::close(int8_t id, int32_t pauseMs) {
  bool pause;
  {
    Lock l;
    const uint32_t t = clockMs();
    s_core.close(id, t, pauseMs);
    pause = !s_core.windowOpen(t);
  }
  if (pause && s_waiting.load()) wakeAll();
}

bool WriteGate::tryEnter(uint32_t since, bool onlyFree, uint32_t* heldSince) {
  Lock l;
  const WriteGateCore::Decision d = s_core.enter(clockMs(), since, onlyFree);
  if (heldSince) *heldSince = d.at;
  return d.verdict == WriteGateCore::Verdict::Go;
}

bool WriteGate::ready(uint32_t since, bool onlyFree) {
  Lock l;
  return s_core.wouldGo(clockMs(), since, onlyFree);
}

bool WriteGate::enter(uint32_t boundMs, const std::atomic<bool>* hurry, uint32_t since, uint32_t* heldSince) {
  Sems& s = sems();
  const uint32_t start = now();  // the bound counts from here
  if (!since) since = start;     // the wait (maxWaitMs) may count from earlier
  int8_t slot = -1;
  WriteGateCore::Ticket ticket = 0;
  ++s_waiting;
  {
    Lock l;
    ticket = s_core.join(start);  // the queue keeps its place across wake-ups
    for (uint8_t i = 0; i < kWaiters; ++i) {
      if (!s_used[i]) {
        s_used[i] = true;
        slot = static_cast<int8_t>(i);
        break;
      }
    }
  }
  SemaphoreHandle_t sem = s.sem[slot >= 0 ? slot : kWaiters];
  if (slot >= 0) (void)xSemaphoreTake(sem, 0);  // a give left over from its last user
  for (;;) {
    WriteGateCore::Decision d;
    uint32_t t;
    {
      Lock l;
      t = clockMs();
      const bool late = (boundMs && t - start >= boundMs) || (hurry && hurry->load());
      d = s_core.enter(t, since, false, ticket);
      if (d.verdict == WriteGateCore::Verdict::Wait && late) d = s_core.force(t, since);
      if (d.verdict != WriteGateCore::Verdict::Wait) {
        s_core.quit(ticket);
        if (slot >= 0) s_used[slot] = false;
      }
    }
    if (d.verdict != WriteGateCore::Verdict::Wait) {
      --s_waiting;
      if (heldSince) *heldSince = d.at;
      // The next in the queue may go too (free gate: several at once).
      if (d.verdict == WriteGateCore::Verdict::Go && s_waiting.load()) wakeAll();
      return d.verdict == WriteGateCore::Verdict::Go;
    }
    uint32_t ms = d.retryMs;
    if (boundMs) {
      const uint32_t gone = t - start;
      const uint32_t left = gone < boundMs ? boundMs - gone : 1;
      if (left < ms) ms = left;
    }
    if (ms > 1000) ms = 1000;  // a bounded wait: a missed edge costs a second at most
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    (void)xSemaphoreTake(sem, ticks ? ticks : 1);
  }
}

void WriteGate::leave(uint32_t heldSince) {
  {
    Lock l;
    s_core.leave(clockMs(), heldSince);
  }
  if (s_waiting.load()) wakeAll();
}

void WriteGate::noteDeferred() {
  Lock l;
  s_core.noteDeferred();
}

void WriteGate::nudge() { wakeAll(); }

uint8_t WriteGate::waiting() { return s_waiting.load(); }

uint8_t WriteGate::writing() {
  Lock l;
  return s_core.writing();
}

bool WriteGate::inWindow() {
  Lock l;
  return s_core.windowOpen(clockMs());
}

bool WriteGate::isFree() {
  Lock l;
  return s_core.isFree(clockMs());
}

WriteGateStats WriteGate::stats() {
  Lock l;
  return s_core.stats();
}

void WriteGate::resetStats() {
  Lock l;
  s_core.resetStats();
}

uint8_t WriteGate::holders() {
  Lock l;
  return s_core.holders();
}

const char* WriteGate::holderName(int8_t id) {
  Lock l;
  return s_core.holderName(id);
}

}  // namespace ESPRack
