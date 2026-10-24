#include "system/BootGuard.h"

#include <Arduino.h>
#include <esp_app_desc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_rom_crc.h>
#include <nvs.h>
#include <stddef.h>
#include <string.h>

#include "App.h"
#include "ProblemJson.h"
#include "WebManager.h"

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
bool s_begun = false;
uint8_t s_limit = 4;
uint32_t s_stableAfterMs = 120000;

// markStable (loop task) vs clearSafeMode (admin action, async_tcp).
StaticSemaphore_t s_mxBuf;
SemaphoreHandle_t s_mx = nullptr;
struct Lock {
  Lock() {
    if (!s_mx) s_mx = xSemaphoreCreateMutexStatic(&s_mxBuf);  // begin(): early boot, one task
    xSemaphoreTake(s_mx, portMAX_DELAY);
  }
  ~Lock() { xSemaphoreGive(s_mx); }
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

void store() {
  s_rec.magic = kMagic;
  s_rec.crc = crcOf(s_rec);
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) {
    log_w("[boot] NVS open failed — boot record not saved");
    return;
  }
  if (nvs_set_blob(h, kKey, &s_rec, sizeof(s_rec)) != ESP_OK || nvs_commit(h) != ESP_OK) {
    log_w("[boot] NVS write failed — boot record not saved");
  }
  nvs_close(h);
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
  Lock l;
  if (s_begun) return;
  s_begun = true;
  s_limit = limit ? limit : 1;
  s_stableAfterMs = stableAfterMs;

  const esp_reset_reason_t rr = esp_reset_reason();
  const char* last = lastRestartReason();
  const BootClass cls = classify(rr, last);

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
  store();

  s_info.reason = rr;
  s_info.lastRestart = last;
  s_info.bootClass = cls;
  s_info.bootCount = s_rec.bootCount;
  s_info.stable = false;
  publish();
  log_i("[boot] #%u %s (%s), crash run %u/%u%s", (unsigned)s_rec.bootCount, last, bootClassName(cls),
        (unsigned)s_rec.quick, (unsigned)s_limit, s_info.safeMode ? ", SAFE MODE" : "");
}

const BootInfo& info() { return s_info; }

void markStable() {
  Lock l;
  if (!s_begun || s_info.stable) return;
  s_info.stable = true;
  s_rec.flags |= kStable;
  s_rec.quick = 0;
  memset(s_rec.sinceStable, 0, sizeof(s_rec.sinceStable));
  store();
  publish();
  log_i("[boot] stable");
}

void clearSafeMode() {
  Lock l;
  if (!s_begun) return;
  s_rec.flags &= static_cast<uint8_t>(~kSafe);
  s_rec.quick = 0;
  store();
  publish();
  log_w("[boot] safe mode cleared");
}

void loop() {
  if (s_begun && !s_info.stable && millis() >= s_stableAfterMs) markStable();
}

void registerManifest(WebManager* web) {
  if (!web) return;
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
    clearSafeMode();
    r->send(200, "application/json", "{\"ok\":true}");
  };
  web->registerAction(std::move(spec));
}

}  // namespace BootGuard
}  // namespace ESPRack
