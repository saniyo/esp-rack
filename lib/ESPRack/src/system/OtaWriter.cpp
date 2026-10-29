#include "system/OtaWriter.h"

#include <Arduino.h>
#include <esp_err.h>
#include <esp_flash_encrypt.h>
#include <esp_idf_version.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_rom_md5.h>
#include <sdkconfig.h>
#include <string.h>

#include <atomic>

#include "App.h"
#include "power/PowerGuard.h"
#include "system/FirmwareUpdate.h"

namespace ESPRack {

static_assert(sizeof(md5_context_t) <= 96 && alignof(md5_context_t) <= 8, "OtaWriter::_md5Ctx too small");

namespace {

md5_context_t* md5(uint8_t* p) { return reinterpret_cast<md5_context_t*>(p); }

// The writer that holds the update (static storage: RMW is fine).
std::atomic<const OtaWriter*> s_owner{nullptr};

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseMd5(const char* hex, uint8_t out[16]) {
  if (strlen(hex) != 32) return false;
  for (uint8_t i = 0; i < 16; ++i) {
    const int hi = hexDigit(hex[2 * i]);
    const int lo = hexDigit(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>(hi << 4 | lo);
  }
  return true;
}

}  // namespace

uint32_t OtaWriter::freeSlotSize() {
  const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
  return p ? p->size : 0;
}

bool OtaWriter::busy() { return s_owner.load() != nullptr; }

bool OtaWriter::beginWrites() {
  // IDF 5.5 esp_ota_begin: esp_ota_invalidate_inactive_ota_data_slot()
  // with rollback enabled (an otadata sector erase); 5.4 has no such call.
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
  return true;
#else
  return false;
#endif
}

bool OtaWriter::endWrites() { return esp_flash_encryption_enabled(); }

bool OtaWriter::claim(const char* source) {
  if (_source) return true;  // held already
  const OtaWriter* none = nullptr;
  if (!s_owner.compare_exchange_strong(none, this)) return false;  // another OtaWriter
  const char* src = source ? source : "update";
  if (!FirmwareUpdate::tryBegin(src)) {  // another writer (ArduinoOTA, a consumer's)
    s_owner.store(nullptr);
    return false;
  }
  _source = src;
  return true;
}

void OtaWriter::unclaim() {
  if (!_source) return;
  const char* src = _source;
  _source = nullptr;
  FirmwareUpdate::end(src);  // only if still ours (another writer may have taken a stale one)
  s_owner.store(nullptr);  // ours: nobody else changes it while held
}

bool OtaWriter::reserve(const char* source) {
  if (_source) return false;  // reserved, or an image under way
  _error = claim(source) ? 0 : kErrBusy;
  return _error == 0;
}

bool OtaWriter::begin(const char* source, uint32_t sizeHint, const char* md5Hex) {
  if (_handle) (void)esp_ota_abort(_handle);  // the update stays this writer's
  _handle = 0;
  _error = 0;
  _written = 0;
  _erases = 0;
  _ended = false;
  _md5 = false;
  if (!claim(source)) return fail(kErrBusy);
  const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
  _part = part;
  _slotSize = part ? part->size : 0;
  if (!part) return fail(ESP_ERR_NOT_FOUND);
  if (sizeHint > part->size) return fail(kErrTooBig);
  if (md5Hex && *md5Hex) {
    if (!parseMd5(md5Hex, _md5Want)) return fail(kErrBadMd5);
    _md5 = true;
    esp_rom_md5_init(md5(_md5Ctx));
  }
  // Sequential: each sector is erased when the first write reaches it, so
  // begin erases nothing (a size would erase the whole image here, in 64 KB
  // blocks; OTA_SIZE_UNKNOWN the whole slot).
  esp_ota_handle_t h = 0;
  const esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
  if (e != ESP_OK) return fail(e);
  _handle = h;
  FirmwareUpdate::progress();
  return true;
}

bool OtaWriter::write(const uint8_t* data, size_t len) {
  if (!_handle || _ended) return fail(kErrNotBegun);
  if (len > kStep) return fail(kErrStep);
  if (!len) return true;
  if (_written + len > _slotSize) return fail(kErrTooBig);
  const esp_err_t e = esp_ota_write(_handle, data, len);  // checks the image's first byte too
  if (e != ESP_OK) return fail(e);
  if (_md5) esp_rom_md5_update(md5(_md5Ctx), data, static_cast<uint32_t>(len));
  _erases += FlashSteps::erases(_written, static_cast<uint32_t>(len));
  _written += static_cast<uint32_t>(len);
  FirmwareUpdate::progress();
  return true;
}

bool OtaWriter::end() {
  if (!_handle || _ended) return fail(kErrNotBegun);
  if (_md5) {
    uint8_t got[16];
    esp_rom_md5_final(got, md5(_md5Ctx));
    if (memcmp(got, _md5Want, sizeof(got)) != 0) return fail(kErrMd5);
  }
  const esp_ota_handle_t h = _handle;
  _handle = 0;  // esp_ota_end frees it, whatever it says
  const esp_err_t e = esp_ota_end(h);
  if (e != ESP_OK) return fail(e);
  _ended = true;
  return true;
}

bool OtaWriter::activate() {
  if (!_ended || !_part) return fail(kErrNotBegun);
  const esp_err_t e = esp_ota_set_boot_partition(static_cast<const esp_partition_t*>(_part));
  if (e != ESP_OK) return fail(e);
  // The update stays this writer's: the restart that follows boots it, no
  // other writer may take the slot meanwhile.
  log_i("[ota] %u B written (%u sector erases): boots at the next restart", (unsigned)_written,
        (unsigned)_erases);
  return true;
}

void OtaWriter::restartInto(const char* reason) {
  if (!_ended) restartDevice(reason);
  restartDevice(reason, [this]() {
    // The buses are stopped (every onShutdown ran): the image check and
    // the otadata erase hold up nobody here.
    if (!PowerGuard::writesAllowed()) {
      log_e("[ota] flash writes paused (power): the new image is not set to boot");
      return;
    }
    (void)activate();
  });
}

void OtaWriter::abort() {
  if (_handle) (void)esp_ota_abort(_handle);
  _handle = 0;
  _ended = false;
  unclaim();
}

bool OtaWriter::fail(int err) {
  if (!_error) _error = err;
  abort();
  log_e("[ota] %s", errorString());
  return false;
}

UploadSink OtaWriter::sink(const char* source, uint32_t sizeHint) {
  UploadSink s;
  s.open = [this, source, sizeHint]() { return begin(source, sizeHint); };
  s.write = [this](const uint8_t* data, size_t len) { return write(data, len); };
  // Checked and closed; set to boot only by restartInto (the caller, once
  // the pump says Ok): a failure meanwhile aborts it.
  s.finish = [this]() -> int { return end() ? UploadSink::kDone : UploadSink::kFailed; };
  s.abort = [this]() { abort(); };
  s.noFlash = UploadSink::kAbortNoFlash;
  if (!beginWrites()) s.noFlash |= UploadSink::kOpenNoFlash;
  if (!endWrites()) s.noFlash |= UploadSink::kFinishNoFlash;
  s.revocable = true;
  return s;
}

const char* OtaWriter::errorString() const {
  switch (_error) {
    case 0:
      return "";
    case kErrMd5:
      return "MD5 check failed";
    case kErrBadMd5:
      return "bad MD5 given";
    case kErrTooBig:
      return "image larger than the OTA slot";
    case kErrStep:
      return "write longer than 4 KB";
    case kErrNotBegun:
      return "no image under way";
    case kErrBusy:
      return "another firmware update is under way";
    default:
      return esp_err_to_name(static_cast<esp_err_t>(_error));
  }
}

}  // namespace ESPRack
