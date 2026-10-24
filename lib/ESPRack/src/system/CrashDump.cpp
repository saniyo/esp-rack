#include "system/CrashDump.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <esp_app_desc.h>
#include <esp_core_dump.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <memory>
#include <new>

#include "WebManager.h"

namespace ESPRack {
namespace CrashDump {

namespace {
Info s_info;
bool s_cached = false;
StaticSemaphore_t s_mxBuf;
SemaphoreHandle_t s_mx = nullptr;

struct Lock {
  Lock() {
    if (!s_mx) s_mx = xSemaphoreCreateMutexStatic(&s_mxBuf);
    xSemaphoreTake(s_mx, portMAX_DELAY);
  }
  ~Lock() { xSemaphoreGive(s_mx); }
};

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

void sendFile(AsyncWebServerRequest* r) {
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
      "application/octet-stream", size, [part, offset, size](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
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
  if (!s_cached) {
    load(s_info);
    s_cached = true;
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

bool erase() {
  Lock l;
  const bool ok = esp_core_dump_image_erase() == ESP_OK;
  s_cached = false;
  return ok;
}

void registerManifest(WebManager* web) {
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
    spec.successMessage = "Crash dump erased";
    spec.handler = [](AsyncWebServerRequest* r) {
      const bool ok = erase();
      r->send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    };
    web->registerAction(std::move(spec));
  }
}

}  // namespace CrashDump
}  // namespace ESPRack
