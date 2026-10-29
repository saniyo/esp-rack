#include "system/CrashDump.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <esp_app_desc.h>
#include <esp_core_dump.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <memory>
#include <new>

#include "WebManager.h"
#include "power/FlashWriter.h"
#include "power/PowerGuard.h"
#include "power/WriteGate.h"

// See CrashDump.h: IDF clears its "partition in use" flag only in its own
// one-call erase, not in this one sector by sector.
#if CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE
#warning "CrashDump: with CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE a crash after erase() is saved only from the next boot on"
#endif

namespace ESPRack {
namespace CrashDump {

namespace {
Info s_info;
bool s_cached = false;
uint32_t s_cachedGen = 0;
std::atomic<uint32_t> s_gen{0};  // bumped by each erase step: the cached summary is stale
StaticSemaphore_t s_mxBuf;
SemaphoreHandle_t s_mx = nullptr;
constexpr TickType_t kLockWait = pdMS_TO_TICKS(500);

// The summary cache (info(): the System page on async_tcp, any task). Held
// while the dump is read (flash reads, tens of ms), never while it is
// erased. Taken with a timeout: then the last summary stands.
struct Lock {
  Lock() {
    if (!s_mx) s_mx = xSemaphoreCreateMutexStatic(&s_mxBuf);
    ok = xSemaphoreTake(s_mx, kLockWait) == pdTRUE;
  }
  ~Lock() {
    if (ok) xSemaphoreGive(s_mx);
  }
  bool ok;
};

// The erase, a sector per write gate slot (see CrashDump.h).
portMUX_TYPE s_eraseMux = portMUX_INITIALIZER_UNLOCKED;
EraseStatus s_erase;
bool s_stepping = false;  // a sector is being erased
int8_t s_job = -1;
// Downloads under way (an erase waits for none: it is refused meanwhile,
// and a download is refused while an erase runs).
uint8_t s_downloads = 0;

enum class Start : uint8_t { Started, NoPartition, NoJob, Paused, Downloading };

const esp_partition_t* partition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
}

uint32_t sectorOf(const esp_partition_t* p) { return p->erase_size ? p->erase_size : 4096; }

// The next sector: the caller holds a gate pass or slot. false: no more to
// do (done, failed, or no erase under way).
bool eraseStep() {
  portENTER_CRITICAL(&s_eraseMux);
  const bool on = s_erase.state == EraseState::Erasing;
  const bool busy = s_stepping;  // one step at a time
  if (on && !busy) s_stepping = true;
  const uint16_t i = s_erase.done;
  const uint16_t total = s_erase.total;
  portEXIT_CRITICAL(&s_eraseMux);
  if (!on) return false;
  if (busy) return true;
  const esp_partition_t* part = partition();
  esp_err_t e = ESP_ERR_NOT_FOUND;
  if (part && i < total) {
    const uint32_t sec = sectorOf(part);
    e = esp_partition_erase_range(part, size_t(i) * sec, sec);
    if (e == ESP_OK && i == 0) {
      // As esp_core_dump_image_erase(): a blank size word at the start, in
      // 16 bytes (encrypted flash is written in blocks of 16).
      const uint32_t blank[4] = {0xFFFFFFFFu, 0, 0, 0};
      e = esp_partition_write(part, 0, blank, sizeof blank);
    }
  }
  s_gen.store(s_gen.load() + 1);  // one writer: the step that holds the slot
  portENTER_CRITICAL(&s_eraseMux);
  if (e != ESP_OK) s_erase.state = EraseState::Failed;
  else if (++s_erase.done >= s_erase.total) s_erase.state = EraseState::Done;
  s_stepping = false;
  const EraseState st = s_erase.state;
  portEXIT_CRITICAL(&s_eraseMux);
  if (e != ESP_OK) {
    log_w("[coredump] erase failed at sector %u/%u: %s", unsigned(i + 1), unsigned(total), esp_err_to_name(e));
  } else if (st == EraseState::Done) {
    log_i("[coredump] erased (%u sectors)", unsigned(total));
  }
  return st == EraseState::Erasing;
}

// The FlashWriter task: one sector per run, so the other jobs go in
// between. A fresh wait clock per sector: nothing waits on this erase.
uint32_t eraseJob() {
  if (eraseStatus().state != EraseState::Erasing) return FlashWriter::kDone;
  WriteSlot slot;
  if (!slot.ok()) return FlashWriter::kOnPower;  // PowerGuard: resumes where it stopped
  return eraseStep() ? 1 : FlashWriter::kDone;
}

void load(Info& i) {
  i = Info{};
  size_t addr = 0, size = 0;
  if (esp_core_dump_image_check() != ESP_OK || esp_core_dump_image_get(&addr, &size) != ESP_OK) return;
  i.present = true;
  i.size = uint32_t(size);
#if CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
  // About 1 KB (the RISC-V stack dump): off the caller's stack.
  std::unique_ptr<esp_core_dump_summary_t> s(new (std::nothrow) esp_core_dump_summary_t());
  if (s && esp_core_dump_get_summary(s.get()) == ESP_OK) {
    strlcpy(i.task, s->exc_task, sizeof i.task);
    i.pc = s->exc_pc;
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    const uint32_t n = s->exc_bt_info.depth < 8 ? s->exc_bt_info.depth : 8;
    for (uint32_t k = 0; k < n; ++k) i.bt[k] = s->exc_bt_info.bt[k];
    i.depth = uint8_t(n);
#else
    i.bt[0] = s->ex_info.ra;
    i.depth = 1;
#endif
    strlcpy(i.elfSha, reinterpret_cast<const char*>(s->app_elf_sha256), sizeof i.elfSha);
    char mine[sizeof i.elfSha] = {0};
    esp_app_get_elf_sha256(mine, sizeof mine);
    const size_t n2 = strlen(i.elfSha) < strlen(mine) ? strlen(i.elfSha) : strlen(mine);
    i.thisFirmware = n2 > 0 && strncmp(i.elfSha, mine, n2) == 0;
  }
  char reason[sizeof i.reason];
  if (esp_core_dump_get_panic_reason(reason, sizeof reason) == ESP_OK) strlcpy(i.reason, reason, sizeof i.reason);
#endif
}

// A download under way, counted while its response lives (sent, or the
// client gone): the erase is refused meanwhile.
struct Download {
  Download() = default;
  ~Download() {
    portENTER_CRITICAL(&s_eraseMux);
    if (s_downloads) --s_downloads;
    portEXIT_CRITICAL(&s_eraseMux);
  }
  Download(const Download&) = delete;
  Download& operator=(const Download&) = delete;
};

void sendFile(AsyncWebServerRequest* r) {
  // Not while an erase runs: the file would have erased sectors in it.
  bool erasing;
  portENTER_CRITICAL(&s_eraseMux);
  erasing = s_erase.state == EraseState::Erasing;
  if (!erasing) ++s_downloads;
  portEXIT_CRITICAL(&s_eraseMux);
  if (erasing) {
    r->send(409, "application/json", "{\"error\":\"the crash dump is being erased\"}");
    return;
  }
  std::shared_ptr<Download> dl(new (std::nothrow) Download());
  if (!dl) {
    portENTER_CRITICAL(&s_eraseMux);
    --s_downloads;
    portEXIT_CRITICAL(&s_eraseMux);
    r->send(503, "application/json", "{\"error\":\"out of memory\"}");
    return;
  }
  size_t addr = 0, size = 0;
  const esp_partition_t* part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  if (!part || esp_core_dump_image_check() != ESP_OK || esp_core_dump_image_get(&addr, &size) != ESP_OK ||
      addr < part->address || addr + size > part->address + part->size) {
    r->send(404, "application/json", "{\"error\":\"no crash dump\"}");
    return;
  }
  const size_t offset = addr - part->address;
  AsyncWebServerResponse* resp = r->beginResponse(
      "application/octet-stream", size, [part, offset, size, dl](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
        if (index >= size) return 0;
        const size_t n = maxLen < size - index ? maxLen : size - index;
        return esp_partition_read(part, offset + index, buf, n) == ESP_OK ? n : 0;
      });
  resp->addHeader("Content-Disposition", "attachment; filename=\"coredump.bin\"");
  r->send(resp);
}
}  // namespace

const Info& info() {
  Lock l;
  if (!l.ok) return s_info;  // another task is reading the dump: the last summary
  const uint32_t gen = s_gen.load();
  if (!s_cached || s_cachedGen != gen) {
    load(s_info);
    s_cached = true;
    s_cachedGen = gen;
  }
  return s_info;
}

bool describe(char* out, size_t len) {
  if (!out || !len) return false;
  out[0] = 0;
  const Info& i = info();
  if (!i.present) return false;
  size_t n = snprintf(out, len, "%s%s%s, PC 0x%08lx", i.task[0] ? i.task : "?", i.reason[0] ? ": " : "", i.reason,
                      (unsigned long)i.pc);
  for (uint8_t k = 0; k < i.depth && n < len; ++k) {
    n += snprintf(out + n, len - n, "%s0x%08lx", k ? " " : ", bt ", (unsigned long)i.bt[k]);
  }
  if (n < len) snprintf(out + n, len - n, "%s", i.thisFirmware ? " (this firmware)" : " (another firmware)");
  return true;
}

namespace {
Start startErase() {
  const esp_partition_t* part = partition();
  if (!part) {
    log_w("[coredump] no coredump partition");
    return Start::NoPartition;
  }
  if (s_job < 0) {
    log_w("[coredump] no FlashWriter job: not erased");
    return Start::NoJob;
  }
  const bool power = PowerGuard::writesAllowed();
  bool start = false, busy = false;
  portENTER_CRITICAL(&s_eraseMux);
  if (s_erase.state != EraseState::Erasing) {
    // Refused while a download reads it, and while PowerGuard pauses
    // writes (as Factory Reset): one under way goes on once it allows them.
    busy = s_downloads > 0;
    if (!busy && power) {
      s_erase.state = EraseState::Erasing;
      s_erase.done = 0;
      s_erase.total = uint16_t(part->size / sectorOf(part));
      start = true;
    }
  }
  const bool under = s_erase.state == EraseState::Erasing;
  portEXIT_CRITICAL(&s_eraseMux);
  if (busy) return Start::Downloading;
  if (!start) return under ? Start::Started : Start::Paused;
  // The first sector at once when the gate lets it (one bounded step; the
  // dump is invalid from then on), the rest in the job.
  bool more = true;
  {
    WritePass pass;
    if (pass.ok()) more = eraseStep();
    else if (PowerGuard::writesAllowed()) WriteGate::noteDeferred();
  }
  if (more) FlashWriter::kick(s_job);
  return Start::Started;
}
}  // namespace

bool erase() {
  return startErase() == Start::Started && eraseStatus().state != EraseState::Failed;
}

EraseStatus eraseStatus() {
  portENTER_CRITICAL(&s_eraseMux);
  const EraseStatus s = s_erase;
  portEXIT_CRITICAL(&s_eraseMux);
  return s;
}

const char* eraseStateName(EraseState s) {
  switch (s) {
    case EraseState::Idle: return "idle";
    case EraseState::Erasing: return "erasing";
    case EraseState::Done: return "done";
    case EraseState::Failed: return "failed";
  }
  return "?";
}

void registerManifest(WebManager* web) {
  if (!s_mx) s_mx = xSemaphoreCreateMutexStatic(&s_mxBuf);  // setup: before any page asks
  // The erase steps (setup, the loop task).
  if (s_job < 0) {
    s_job = FlashWriter::add("coredump", [] { return eraseJob(); });
    if (s_job < 0) log_w("[coredump] no FlashWriter job left: erase unavailable");
  }
  if (!web) return;
  {
    WebActionSpec spec;
    spec.id = "system.coredump";
    spec.title = "Download the crash dump";
    spec.icon = "Download";
    spec.method = "GET";
    spec.auth = WebAuthLevel::Admin;
    spec.download = "coredump.bin";
    spec.handler = [](AsyncWebServerRequest* r) { sendFile(r); };
    web->registerAction(std::move(spec));
  }
  {
    WebActionSpec spec;
    spec.id = "system.eraseCoredump";
    spec.title = "Erase the crash dump";
    spec.icon = "Delete";
    spec.color = "warning";
    spec.method = "POST";
    spec.auth = WebAuthLevel::Admin;
    spec.confirm = "Erase the crash dump? Download it first if it is still needed.";
    spec.successMessage = "Erasing the crash dump";
    spec.handler = [](AsyncWebServerRequest* r) {
      // Never the whole partition here: a sector at most, the rest in the
      // FlashWriter job. 202 while it runs, 200 once done; 409 while a
      // download reads it, 503 while PowerGuard pauses writes.
      const Start st = startErase();
      if (st == Start::Downloading) {
        r->send(409, "application/json", "{\"ok\":false,\"error\":\"the crash dump is being downloaded\"}");
        return;
      }
      if (st == Start::Paused) {
        r->send(503, "application/json", "{\"ok\":false,\"error\":\"flash writes paused\"}");
        return;
      }
      const EraseStatus s = eraseStatus();
      const bool ok = st == Start::Started && s.state != EraseState::Failed;
      char body[96];
      snprintf(body, sizeof body, "{\"ok\":%s,\"state\":\"%s\",\"done\":%u,\"of\":%u}", ok ? "true" : "false",
               eraseStateName(s.state), unsigned(s.done), unsigned(s.total));
      r->send(!ok ? 500 : s.state == EraseState::Done ? 200 : 202, "application/json", body);
    };
    web->registerAction(std::move(spec));
  }
}

}  // namespace CrashDump
}  // namespace ESPRack
