#include "system/TlsMemory.h"

#ifdef ESP32
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <mbedtls/platform.h>
#endif

namespace ESPRack {
namespace TlsMemory {

namespace {
bool s_psram = false;

#if defined(ESP32) && defined(MBEDTLS_PLATFORM_MEMORY) && !defined(MBEDTLS_PLATFORM_CALLOC_MACRO)
constexpr uint32_t kPsram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr uint32_t kInternal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

void* tlsCalloc(size_t n, size_t size) {
  if (n && size > SIZE_MAX / n) return nullptr;
  const bool big = n * size >= ESPRACK_TLS_PSRAM_MIN;
  void* p = heap_caps_calloc(n, size, big ? kPsram : kInternal);
  if (!p) p = heap_caps_calloc(n, size, big ? kInternal : kPsram);
  return p;
}

void tlsFree(void* p) { heap_caps_free(p); }  // any heap
#define ESPRACK_TLS_MEMORY_HOOK 1
#endif
}  // namespace

bool begin() {
#if ESPRACK_TLS_PSRAM && defined(ESPRACK_TLS_MEMORY_HOOK)
  if (s_psram) return true;
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) return false;  // no PSRAM: as built
  // Blocks taken before this keep working: tlsFree frees from any heap.
  s_psram = mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree) == 0;
  if (s_psram) ESP_LOGI("tls", "mbedTLS blocks >= %u B in PSRAM", (unsigned)ESPRACK_TLS_PSRAM_MIN);
#endif
  return s_psram;
}

bool usingPsram() { return s_psram; }

}  // namespace TlsMemory
}  // namespace ESPRack
