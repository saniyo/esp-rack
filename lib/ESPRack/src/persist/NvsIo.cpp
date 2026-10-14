// NvsRecord's NVS I/O (target only; host tests bring a fake).
#include <nvs.h>

#include "persist/NvsRecord.h"

namespace ESPRack {
namespace nvsrec {

Io get(const char* ns, const char* key, uint8_t* buf, size_t cap, size_t& size) {
  nvs_handle_t h;
  esp_err_t e = nvs_open(ns, NVS_READONLY, &h);
  if (e == ESP_ERR_NVS_NOT_FOUND) return Io::Missing;  // the namespace doesn't exist yet
  if (e != ESP_OK) return Io::NoStore;
  size_t n = 0;
  e = nvs_get_blob(h, key, nullptr, &n);
  if (e != ESP_OK) {
    nvs_close(h);
    return e == ESP_ERR_NVS_NOT_FOUND ? Io::Missing : Io::NoStore;
  }
  size = n;
  if (n > cap) {
    nvs_close(h);
    return Io::TooBig;
  }
  e = nvs_get_blob(h, key, buf, &n);
  nvs_close(h);
  return e == ESP_OK ? Io::Ok : Io::NoStore;
}

Io set(const char* ns, const char* key, const uint8_t* buf, size_t size) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return Io::NoStore;
  esp_err_t e = nvs_set_blob(h, key, buf, size);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK ? Io::Ok : Io::Failed;
}

}  // namespace nvsrec
}  // namespace ESPRack
