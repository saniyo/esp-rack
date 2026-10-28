// WriteGate on the host: one thread, a clock the test sets
// (writeGateHostSetNow). enter() can't block here: where the device would
// wait, it goes as if its bound ran out (forced).

#include <power/WriteGate.h>

#include "WriteGateHost.h"

namespace ESPRack {

namespace {
WriteGateCore s_core;
uint32_t s_now = 0;
}  // namespace

void writeGateHostSetNow(uint32_t ms) { s_now = ms; }
void writeGateHostReset() {
  s_core = WriteGateCore{};
  s_now = 0;
}

uint32_t WriteGate::now() { return s_now; }
void WriteGate::configure(const WriteGateConfig& c) { s_core.configure(c); }
WriteGateConfig WriteGate::config() { return s_core.config(); }
int8_t WriteGate::addHolder(const char* name) { return s_core.addHolder(name); }
void WriteGate::open(int8_t id) { s_core.open(id, s_now); }
void WriteGate::close(int8_t id) { s_core.close(id, s_now); }
void WriteGate::close(int8_t id, int32_t pauseMs) { s_core.close(id, s_now, pauseMs); }

bool WriteGate::tryEnter(uint32_t since, bool onlyFree, uint32_t* heldSince) {
  const WriteGateCore::Decision d = s_core.enter(s_now, since, onlyFree);
  if (heldSince) *heldSince = d.at;
  return d.verdict == WriteGateCore::Verdict::Go;
}

bool WriteGate::ready(uint32_t since, bool onlyFree) { return s_core.wouldGo(s_now, since, onlyFree); }

bool WriteGate::enter(uint32_t, const std::atomic<bool>*, uint32_t since, uint32_t* heldSince) {
  if (!since) since = s_now;
  WriteGateCore::Decision d = s_core.enter(s_now, since);
  if (d.verdict == WriteGateCore::Verdict::Wait) d = s_core.force(s_now, since);
  if (heldSince) *heldSince = d.at;
  return d.verdict == WriteGateCore::Verdict::Go;
}

void WriteGate::leave(uint32_t heldSince) { s_core.leave(s_now, heldSince); }
void WriteGate::noteDeferred() { s_core.noteDeferred(); }
void WriteGate::nudge() {}
uint8_t WriteGate::waiting() { return s_core.queued(); }
uint8_t WriteGate::writing() { return s_core.writing(); }
bool WriteGate::inWindow() { return s_core.windowOpen(s_now); }
bool WriteGate::isFree() { return s_core.isFree(s_now); }
WriteGateStats WriteGate::stats() { return s_core.stats(); }
void WriteGate::resetStats() { s_core.resetStats(); }
uint8_t WriteGate::holders() { return s_core.holders(); }
const char* WriteGate::holderName(int8_t id) { return s_core.holderName(id); }

}  // namespace ESPRack
