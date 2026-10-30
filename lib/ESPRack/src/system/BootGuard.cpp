#include "system/BootGuard.h"

#include <Arduino.h>
#include <esp_app_desc.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_rom_crc.h>
#include <nvs.h>
#include <stddef.h>
#include <string.h>

#include "App.h"
#include "ProblemJson.h"
#include "WebManager.h"
#include "power/FlashWriter.h"
#include "power/PowerGuard.h"
#include "power/WriteGate.h"
#include "system/LoopWait.h"
#include "system/OtaLoader.h"
#include "time/TimeSync.h"

namespace ESPRack {

namespace {

constexpr int kClasses = static_cast<int>(BootClass::Count);

// NVS blob "esprack"/"boot". 32 bytes.
struct Record {
  uint32_t magic;
  uint32_t bootCount;
  uint8_t quick;
  uint8_t flags;  // kStable | kSafe
  uint8_t sinceStable[kClasses];
  uint8_t elfSha[8];  // first bytes of the firmware's ELF SHA-256
  uint8_t pad[32 - 4 - 4 - 1 - 1 - kClasses - 8 - 4];
  uint32_t crc;
};
static_assert(sizeof(Record) == 32, "boot record layout");
constexpr uint32_t kMagic = 0x31444742u;  // "BGD1"
constexpr uint8_t kStable = 0x01;
constexpr uint8_t kSafe = 0x02;
constexpr const char* kNs = "esprack";
constexpr const char* kKey = "boot";

Record s_rec{};
BootInfo s_info{};
bool s_begun = false;  // begin() was called
bool s_ready = false;  // begin() is done
uint8_t s_limit = 4;
uint32_t s_stableAfterMs = 120000;

// markStable (loop task) vs clearSafeMode (admin action, async_tcp) vs the
// FlashWriter task. A spinlock: only copies and flags under it (a few
// hundred bytes at most), never a flash write or a log line, so nobody
// waits on it longer than that.
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
struct Lock {
  Lock() { portENTER_CRITICAL(&s_mux); }
  ~Lock() { portEXIT_CRITICAL(&s_mux); }
};

uint32_t crcOf(const Record& r) {
  return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&r), offsetof(Record, crc));
}

bool load(Record& r) {
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return false;
  size_t len = sizeof(r);
  const bool ok = nvs_get_blob(h, kKey, &r, &len) == ESP_OK && len == sizeof(r) && r.magic == kMagic &&
                  r.crc == crcOf(r);
  nvs_close(h);
  return ok;
}

// One NVS blob: one bounded flash step (a page write, now and then a
// sector erase).
bool nvsPut(const char* key, const void* data, size_t len) {
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = nvs_set_blob(h, key, data, len) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

// r: a copy, sealed here and written outside the lock. false: not saved (logged).
bool store(Record& r) {
  r.magic = kMagic;
  r.crc = crcOf(r);
  if (nvsPut(kKey, &r, sizeof(r))) return true;
  log_w("[boot] NVS write failed — boot record not saved");
  return false;
}

BootClass classify(esp_reset_reason_t rr, const char* last) {
  switch (rr) {
    case ESP_RST_POWERON: return BootClass::PowerOn;
    case ESP_RST_BROWNOUT: return BootClass::Brownout;
    case ESP_RST_PANIC: return BootClass::Panic;
    case ESP_RST_INT_WDT: return BootClass::IntWdt;
    case ESP_RST_TASK_WDT: return BootClass::TaskWdt;
    case ESP_RST_WDT: return BootClass::Wdt;
    case ESP_RST_SW: return strcmp(last, "idf") == 0 ? BootClass::Unexplained : BootClass::Intended;
    case ESP_RST_EXT:
    case ESP_RST_DEEPSLEEP:
    case ESP_RST_USB:
    case ESP_RST_JTAG: return BootClass::Other;
    default: return BootClass::Unexplained;
  }
}

// NVS blob "esprack"/"boots": the last boots, slot head = the newest.
struct HistRec {
  uint32_t bootCount;
  uint32_t bootT;
  uint32_t prevUpS;
  uint8_t cls;
  char reason[24];  // restart reasons are at most 23 chars (App.h)
  uint8_t pad[3];
};
static_assert(sizeof(HistRec) == 40, "boot history record layout");
struct History {
  uint32_t magic;
  uint8_t head;
  uint8_t pad[3];
  HistRec rec[kBootHistory];
  uint32_t crc;
};
constexpr uint32_t kHistMagic = 0x31484742u;  // "BGH1"
constexpr const char* kHistKey = "boots";
History s_hist{};
bool s_timed = false;  // this boot's time is in the history

// The run's uptime, kept across a soft reset in RTC memory (not a power cut).
struct UpRecord {
  uint32_t magic;
  uint32_t upS;
  uint32_t crc;
};
constexpr uint32_t kUpMagic = 0x31505542u;  // "BUP1"
RTC_NOINIT_ATTR UpRecord s_up;
uint32_t s_lastUpS = 0;

void storeUptime(uint32_t s) {
  UpRecord r{kUpMagic, s, 0};
  r.crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&r), offsetof(UpRecord, crc));
  s_up = r;
}

uint32_t previousUptime() {
  const UpRecord r = s_up;
  const bool ok = r.magic == kUpMagic &&
                  r.crc == esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&r), offsetof(UpRecord, crc));
  return ok ? r.upS : 0;
}

// h: a copy, sealed here and written outside the lock. false: not saved (logged).
bool storeHistory(History& h) {
  h.magic = kHistMagic;
  h.crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&h), offsetof(History, crc));
  if (nvsPut(kHistKey, &h, sizeof(h))) return true;
  log_w("[boot] NVS write failed — boot history not saved");
  return false;
}

void loadHistory() {
  nvs_handle_t h;
  bool ok = false;
  if (nvs_open(kNs, NVS_READONLY, &h) == ESP_OK) {
    size_t len = sizeof(s_hist);
    ok = nvs_get_blob(h, kHistKey, &s_hist, &len) == ESP_OK && len == sizeof(s_hist) &&
         s_hist.magic == kHistMagic && s_hist.head < kBootHistory &&
         s_hist.crc == esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&s_hist), offsetof(History, crc));
    nvs_close(h);
  }
  if (!ok) s_hist = History{};
}

// The writes after the boot's own go through the flash write gate
// (power/WriteGate.h). A change bumps its blob's generation; a writer copies
// the blob under the lock, writes the copy outside it and then marks that
// generation saved. One writer at a time, so an older copy never lands
// after a newer one. On the shared tasks (the loop: markStable, the boot's
// time; async_tcp: clearSafeMode) a gate pass, never a wait: when the gate
// says no, the FlashWriter job "bootguard" writes it, a slot per blob.
// While PowerGuard pauses writes the change waits in RAM, and the wait
// clock stops (it starts again when the job runs again). Without a job
// (FlashWriter full) only gate passes write: the next save(), loop() or
// flush() tries again, never past the gate.
struct Gen {
  uint32_t now = 0;
  uint32_t saved = 0;
  bool dirty() const { return saved != now; }
};
Gen s_recGen, s_histGen;
bool s_writing = false;
uint32_t s_putOffAt = 0;  // gate clock: when the oldest pending write was put off; 0 = none
int8_t s_job = -1;
// Shutdown: flush() writes what is left, the job takes no new blob; a
// writer at it when flush() comes gives s_done once it is done.
bool s_flushing = false;
bool s_flushWaits = false;
StaticSemaphore_t s_doneBuf;
SemaphoreHandle_t s_done = nullptr;

bool pending() {
  Lock l;
  return s_recGen.dirty() || s_histGen.dirty();
}

enum class Put : uint8_t { Nothing, Busy, Done, Failed };

// One pending blob, written now: the caller holds a gate pass or slot.
// A failed write is logged, reported (Failed) and not tried again (as at
// boot). wait: on Busy, the writer at it gives s_done once it is done.
Put writeOne(bool wait = false) {
  Record rec;
  History hist;
  bool isRec = false;
  uint32_t gen = 0;
  {
    Lock l;
    if (s_writing) {
      if (wait) s_flushWaits = true;
      return Put::Busy;
    }
    if (s_recGen.dirty()) {
      isRec = true;
      gen = s_recGen.now;
      rec = s_rec;
    } else if (s_histGen.dirty()) {
      gen = s_histGen.now;
      hist = s_hist;
    } else {
      s_putOffAt = 0;
      return Put::Nothing;
    }
    s_writing = true;
  }
  const bool ok = isRec ? store(rec) : storeHistory(hist);
  bool wake;
  {
    Lock l;
    (isRec ? s_recGen : s_histGen).saved = gen;
    s_writing = false;
    if (!s_recGen.dirty() && !s_histGen.dirty()) s_putOffAt = 0;
    wake = s_flushWaits;
    s_flushWaits = false;
  }
  if (wake && s_done) xSemaphoreGive(s_done);
  return ok ? Put::Done : Put::Failed;
}

// The FlashWriter task: what is pending, a gate slot per blob, on the wait
// clock of the first write put off.
uint32_t writeJob() {
  for (;;) {
    const uint32_t t = WriteGate::now();
    uint32_t since;
    {
      Lock l;
      // Shutdown: flush() writes the rest.
      if (s_flushing || (!s_recGen.dirty() && !s_histGen.dirty())) return FlashWriter::kDone;
      if (!s_putOffAt) s_putOffAt = t;  // put off while PowerGuard paused writes: the clock starts now
      since = s_putOffAt;
    }
    WriteSlot slot(0, nullptr, since);
    if (!slot.ok()) {  // PowerGuard: the change waits in RAM, the wait clock stops
      Lock l;
      s_putOffAt = 0;
      return FlashWriter::kOnPower;
    }
    const Put p = writeOne();
    if (p == Put::Busy) return 10;  // a pass on a shared task is writing: look again
    if (p == Put::Nothing) return FlashWriter::kDone;
  }
}

enum class Saved : uint8_t { Now, PutOff, Failed };

// After a change, on a shared task: never waits. retry: a change put off
// before (no job), not counted again.
Saved save(bool retry = false) {
  uint32_t since;
  {
    Lock l;
    since = s_putOffAt ? s_putOffAt : WriteGate::now();
  }
  bool failed = false;
  // One NVS write per pass. A second blob pending goes to the job, or
  // without one takes a pass of its own (the gate may say no to it).
  for (int i = 0; i < 2; ++i) {
    WritePass pass(since);
    if (!pass.ok()) break;
    const Put p = writeOne();
    if (p == Put::Failed) failed = true;
    if (p == Put::Busy || p == Put::Nothing || s_job >= 0) break;
  }
  if (!pending()) return failed ? Saved::Failed : Saved::Now;
  // Put off. While PowerGuard pauses writes the wait clock doesn't run
  // (writeJob starts it) and nothing is counted as put off.
  const bool power = PowerGuard::writesAllowed();
  {
    Lock l;
    if (!s_putOffAt && power) s_putOffAt = since;
  }
  if (power && !retry) WriteGate::noteDeferred();
  if (s_job >= 0) FlashWriter::kick(s_job);
  return failed ? Saved::Failed : Saved::PutOff;
}

void publish() {
  s_info.quickReboots = s_rec.quick;
  s_info.safeMode = (s_rec.flags & kSafe) != 0;
  memcpy(s_info.sinceStable, s_rec.sinceStable, sizeof(s_info.sinceStable));
}

}  // namespace

const char* bootClassName(BootClass c) {
  static const char* const kNames[kClasses] = {"power-on", "brownout", "panic",    "int-wdt", "task-wdt",
                                               "wdt",      "crash",    "intended", "other"};
  const int i = static_cast<int>(c);
  return i >= 0 && i < kClasses ? kNames[i] : "?";
}

namespace BootGuard {

bool isCrash(BootClass c) {
  return c == BootClass::Panic || c == BootClass::IntWdt || c == BootClass::TaskWdt || c == BootClass::Wdt ||
         c == BootClass::Unexplained;
}

void begin(uint8_t limit, uint32_t windowMs, uint32_t stableAfterMs) {
  (void)windowMs;  // see BootGuard.h
  {
    Lock l;
    if (s_begun) return;
    s_begun = true;
  }
  // Early boot, one task: nobody else touches the state before s_ready.
  s_limit = limit ? limit : 1;
  s_stableAfterMs = stableAfterMs;

  const esp_reset_reason_t rr = esp_reset_reason();
  const char* last = lastRestartReason();
  BootClass cls = classify(rr, last);
  // The update loader restarts into the app with a plain esp_restart
  // ("idf"): it leaves a result, so the boot is an intended one.
  OtaLoader::begin();
  if (cls == BootClass::Unexplained && rr == ESP_RST_SW && OtaLoader::fromLoader()) {
    cls = BootClass::Intended;
    last = "loader";
  }

  if (!load(s_rec)) s_rec = Record{};

  // A new firmware or a factory reset is a new chance: safe mode and the
  // crash run start over.
  uint8_t sha[8] = {0};
  if (const esp_app_desc_t* d = esp_app_get_description()) memcpy(sha, d->app_elf_sha256, sizeof(sha));
  const bool newFirmware = memcmp(sha, s_rec.elfSha, sizeof(sha)) != 0;
  const bool factoryReset = strcmp(last, kRestartFactoryReset) == 0;
  if (newFirmware || factoryReset) {
    if (s_rec.flags & kSafe) log_w("[boot] safe mode cleared (%s)", newFirmware ? "new firmware" : "factory reset");
    s_rec.flags &= static_cast<uint8_t>(~kSafe);
    s_rec.quick = 0;
    memcpy(s_rec.elfSha, sha, sizeof(sha));
  }

  ++s_rec.bootCount;
  uint8_t& n = s_rec.sinceStable[static_cast<int>(cls)];
  if (n < 255) ++n;
  if (isCrash(cls) && s_rec.quick < 255) ++s_rec.quick;
  if (s_rec.quick >= s_limit && !(s_rec.flags & kSafe)) {
    s_rec.flags |= kSafe;
    log_e("[boot] SAFE MODE: %u crash boots in a row (last: %s)", (unsigned)s_rec.quick, last);
  }
  s_rec.flags &= static_cast<uint8_t>(~kStable);
  // The boot's writes go at once: no module holds write windows yet.
  Record rec = s_rec;
  store(rec);

  // This boot into the history; the previous run's length from RTC memory.
  loadHistory();
  s_hist.head = uint8_t((s_hist.head + 1) % kBootHistory);
  HistRec& h = s_hist.rec[s_hist.head];
  h = HistRec{};
  h.bootCount = s_rec.bootCount;
  h.prevUpS = (rr == ESP_RST_POWERON) ? 0 : previousUptime();
  h.cls = uint8_t(cls);
  strlcpy(h.reason, last, sizeof(h.reason));
  History hist = s_hist;
  storeHistory(hist);
  storeUptime(0);

  s_info.reason = rr;
  s_info.lastRestart = last;
  s_info.bootClass = cls;
  s_info.bootCount = s_rec.bootCount;
  s_info.stable = false;
  publish();
  // The writes the gate puts off later (setup, the loop task).
  if (!s_done) s_done = xSemaphoreCreateBinaryStatic(&s_doneBuf);
  if (s_job < 0) {
    s_job = FlashWriter::add("bootguard", [] { return writeJob(); });
    if (s_job < 0) log_w("[boot] no FlashWriter job left: boot record writes only in a gate pass");
  }
  {
    Lock l;
    s_ready = true;
  }
  log_i("[boot] #%u %s (%s), crash run %u/%u%s", (unsigned)s_rec.bootCount, last, bootClassName(cls),
        (unsigned)s_rec.quick, (unsigned)s_limit, s_info.safeMode ? ", SAFE MODE" : "");

  // Loader scheme: a crash run past safe mode (limit + 2) goes to the
  // update loader's upload page, the one way left to replace the app
  // without a cable. A boot back from the loader is no crash and ends no
  // run: every further crash boot goes again, up to a stable run or a new
  // firmware.
  if (isCrash(cls) && s_rec.quick >= uint16_t(s_limit) + 2 && OtaLoader::scheme() == OtaLoader::Scheme::Loader) {
    log_e("[boot] %u crash boots in a row: into the update loader", (unsigned)s_rec.quick);
    ota::Handoff h;  // the Wi-Fi isn't up yet: the loader's own copy
    OtaLoader::makePush(h, nullptr);
    String why;
    if (!OtaLoader::enter(h, kRestartCrashLoop, why)) log_e("[boot] update loader not entered: %s", why.c_str());
  }
}

const BootInfo& info() { return s_info; }

size_t history(BootRecord* out, size_t max) {
  Lock l;
  if (!s_ready) return 0;
  size_t n = 0;
  for (size_t i = 0; i < kBootHistory && n < max; ++i) {
    const HistRec& h = s_hist.rec[(s_hist.head + kBootHistory - i) % kBootHistory];
    if (!h.bootCount) break;
    BootRecord& r = out[n++];
    r.bootCount = h.bootCount;
    r.bootT = h.bootT;
    r.prevUpS = h.prevUpS;
    r.bootClass = h.cls < kClasses ? BootClass(h.cls) : BootClass::Other;
    strlcpy(r.reason, h.reason, sizeof(r.reason));
  }
  return n;
}

void markStable() {
  {
    Lock l;
    if (!s_ready || s_info.stable) return;
    s_info.stable = true;
    s_rec.flags |= kStable;
    s_rec.quick = 0;
    memset(s_rec.sinceStable, 0, sizeof(s_rec.sinceStable));
    ++s_recGen.now;
    publish();
  }
  log_i("[boot] stable");
  (void)save();
}

namespace {
// Clears safe mode in RAM and saves it; out: how (the admin action tells).
bool clearSafe(Saved& out) {
  {
    Lock l;
    if (!s_ready) return false;
    s_rec.flags &= static_cast<uint8_t>(~kSafe);
    s_rec.quick = 0;
    ++s_recGen.now;
    publish();
  }
  log_w("[boot] safe mode cleared");
  out = save();
  return true;
}
}  // namespace

bool clearSafeMode() {
  Saved s = Saved::PutOff;
  return clearSafe(s) && s == Saved::Now;
}

bool savePending() { return pending(); }

void flush(uint32_t boundMs) {
  {
    Lock l;
    s_flushing = true;  // the job takes no new blob from here on
  }
  // Two blobs at most, in 2 x boundMs (App::shutdownGraceMs). A writer at
  // it (the job, a pass on async_tcp) is waited for, its end signalled,
  // then the rest is written here.
  const uint32_t start = WriteGate::now();
  auto left = [&]() -> uint32_t {
    const uint32_t gone = WriteGate::now() - start;
    return gone < 2 * boundMs ? 2 * boundMs - gone : 0;
  };
  for (int i = 0; i < 4 && pending(); ++i) {
    uint32_t bound = 0;  // boundMs 0: the gate's rules
    if (boundMs) {
      bound = left() < boundMs ? left() : boundMs;
      if (!bound) break;
    }
    WriteSlot slot(bound);
    if (!slot.ok()) break;  // PowerGuard: kept in RAM
    const Put p = writeOne(true);
    if (p == Put::Nothing) break;
    if (p != Put::Busy) continue;
    slot.release();
    const uint32_t w = boundMs ? left() : 1000;
    if (!s_done || !w || xSemaphoreTake(s_done, pdMS_TO_TICKS(w) + 1) != pdTRUE) break;
  }
  bool rest;
  {
    Lock l;
    s_flushing = false;
    s_flushWaits = false;
    rest = s_recGen.dirty() || s_histGen.dirty();
  }
  if (rest) {
    log_w("[boot] boot record not saved before the restart (%s)",
          PowerGuard::writesAllowed() ? "no write gate slot in time" : "flash writes paused");
    if (s_job >= 0) FlashWriter::kick(s_job);  // still in RAM: the job goes on if no restart comes
  }
}

void loop() {
  if (!s_ready) return;
  const uint32_t ms = millis();
  if (!s_info.stable && ms >= s_stableAfterMs) markStable();
  // No FlashWriter job: a change the gate put off, in a pass once it lets.
  if (s_job < 0 && PowerGuard::writesAllowed() && pending()) (void)save(true);
  // The run's length for the next boot (RTC memory: no flash wear).
  const uint32_t upS = ms / 1000;
  if (upS - s_lastUpS >= 10) {
    s_lastUpS = upS;
    storeUptime(upS);
  }
  // The time of this boot, once the clock is valid (one NVS write).
  if (!s_timed && TimeSync::instance().isValid()) {
    s_timed = true;
    const time_t now = time(nullptr);
    {
      Lock l;
      s_hist.rec[s_hist.head].bootT = uint32_t(now) - upS;
      ++s_histGen.now;
    }
    (void)save();
  }
}

uint32_t loopWaitMs(uint32_t nowMs) {
  if (!s_ready) return ESPRACK_LOOP_IDLE_WAIT_MS;
  uint32_t w = LoopWait::dueIn(nowMs, s_lastUpS * 1000u, 10000);  // the uptime record
  if (!s_info.stable) {
    const uint32_t st = nowMs >= s_stableAfterMs ? 0 : s_stableAfterMs - nowMs;
    if (st < w) w = st;
  }
  return w;
}

void registerManifest(WebManager* web) {
  if (!web) return;
  // Loader scheme: system.enterUpdateMode, wanted most in safe mode.
  OtaLoader::registerManifest(web);
  WebActionSpec spec;
  spec.id = "system.clearSafeMode";
  spec.title = "Clear safe mode";
  spec.icon = "CheckCircle";
  spec.color = "warning";
  spec.method = "POST";
  spec.auth = WebAuthLevel::Admin;
  spec.confirm = "Leave safe mode? The code that crashed runs again.";
  spec.successMessage = "Safe mode cleared";
  spec.handler = [](AsyncWebServerRequest* r) {
    // Cleared at once; saved now ("saved":true), or by the FlashWriter task
    // once the write gate lets it (false; kept in RAM meanwhile). A failed
    // NVS write: 500, cleared until the restart only.
    Saved s = Saved::PutOff;
    if (!clearSafe(s)) {
      r->send(503, "application/json", "{\"ok\":false,\"error\":\"boot guard not started\"}");
      return;
    }
    if (s == Saved::Failed) {
      r->send(500, "application/json",
              "{\"ok\":false,\"saved\":false,\"error\":\"NVS write failed: cleared until the restart only\"}");
      return;
    }
    r->send(200, "application/json", s == Saved::Now ? "{\"ok\":true,\"saved\":true}" : "{\"ok\":true,\"saved\":false}");
  };
  web->registerAction(std::move(spec));
}

}  // namespace BootGuard
}  // namespace ESPRack
