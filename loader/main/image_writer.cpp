#include "image_writer.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <bootloader_common.h>
#include <esp_app_format.h>
#include <esp_image_format.h>
#include <esp_log.h>
#include <sdkconfig.h>

#include "loader.h"
#include "system/FlashSteps.h"

namespace loader {

namespace {
const char* const TAG = "loader";
ImageWriter s_writer;
}  // namespace

ImageWriter& imageWriter() { return s_writer; }

const char* signatureVerdict(const uint8_t sha[32], const uint8_t* sig, uint8_t sigLen, bool* refuse) {
  const Sig s = sigLen ? verifyP256(pubKey(), sha, sig, sigLen) : Sig::Format;
  *refuse = false;
  if (s == Sig::Ok) return "";
  *refuse = g.prodKey;
  if (!sigLen) return g.prodKey ? "unsigned image" : "unsigned image (dev signing key)";
  return g.prodKey ? sigText(s) : "signature not good for this loader's key (dev signing key)";
}

void ImageWriter::begin(const esp_partition_t* slot, const Expect& expect) {
  abandon();
  _slot = slot;
  _expect = expect;
  _started = false;
  _touched = false;
  _image = 0;
  _fill = 0;
  _head = HeadInfo{};
  _hold.reset();
  _sha.restart();
  _error[0] = 0;
  _warning[0] = 0;
}

bool ImageWriter::feed(const uint8_t* p, size_t n) {
  if (_error[0]) return false;
  return _hold.feed(p, n, [this](const uint8_t* d, size_t k) { return take(d, k); });
}

bool ImageWriter::take(const uint8_t* p, size_t n) {
  if (!_slot) return fail("no app slot");
  if (uint64_t(_image) + n > _slot->size) return fail("image larger than the app slot (%u B)", unsigned(_slot->size));
  if (_expect.size && uint64_t(_image) + n > _expect.size) return fail("image longer than announced");
  while (n) {
    // Up to the next sector boundary: the buffer then holds one whole
    // sector, the image's sectors and the slot's line up (offset 0).
    const uint32_t k = ESPRack::FlashSteps::step(_image, uint32_t(n));
    memcpy(_buf + _fill, p, k);
    _fill += k;
    _image += k;
    p += k;
    n -= k;
    if (_fill == sizeof(_buf) && !flush()) return false;
  }
  return true;
}

bool ImageWriter::flush() {
  if (!_fill) return true;
  const uint32_t at = _image - _fill;  // a sector's start: every flush but the last is a whole one
  if (!_started) {
    // The first sector: everything that can be told from the head before
    // ota_0 is touched.
    const Head h = checkHead(_buf, _fill, CONFIG_IDF_FIRMWARE_CHIP_ID, _head);
    if (h != Head::Ok) return fail("%s", headText(h));
    if (bootloader_common_check_chip_validity(reinterpret_cast<const esp_image_header_t*>(_buf),
                                              ESP_IMAGE_APPLICATION) != ESP_OK)
      return fail("image not for this chip revision");
    _started = true;
    ESP_LOGI(TAG, "image %s (%s): writing", _head.version, _head.project);
  }
  _touched = true;  // this erase is the point of no return (sector 0: ota_0 no longer boots)
  esp_err_t e = esp_partition_erase_range(_slot, at, ESPRack::FlashSteps::kSector);
  if (e == ESP_OK && at == 0) {
    // checkHead saw more than kHeadHold bytes. The head stays erased.
    memcpy(_first, _buf, kHeadHold);
    e = esp_partition_write(_slot, kHeadHold, _buf + kHeadHold, _fill - kHeadHold);
  } else if (e == ESP_OK) {
    e = esp_partition_write(_slot, at, _buf, _fill);
  }
  if (e != ESP_OK) return fail("flash write at %u: 0x%x", unsigned(at), unsigned(e));
  _sha.update(_buf, _fill);
  _fill = 0;
  return true;
}

bool ImageWriter::finish() {
  if (_error[0]) return false;
  // The held-back tail: a signature trailer, or the image's own end.
  const uint32_t fileLen = _image + uint32_t(_hold.held());
  ota::SigTrailer tr = {};
  const ota::TrailerVerdict tv =
      _hold.held() == ota::kTrailerSize ? ota::parseTrailer(_hold.tail(), fileLen, tr) : ota::TrailerVerdict::None;
  if (tv == ota::TrailerVerdict::Corrupt) return fail("signature trailer corrupt");
  if (tv == ota::TrailerVerdict::None && !take(_hold.tail(), _hold.held())) return false;
  _hold.reset();
  if (!flush()) return false;
  if (!_started) return fail("empty image");
  if (!sizeAsAnnounced(_expect.size, _image, tv == ota::TrailerVerdict::Ok))
    return fail("image is %u B, %u announced", unsigned(_image), unsigned(_expect.size));
  uint8_t sha[32];
  _sha.finish(sha);
  if (_expect.sha256 && memcmp(sha, _expect.sha256, sizeof(sha)) != 0) return fail("SHA-256 differs from the handoff's");
  const uint8_t* sig = _expect.sigLen ? _expect.sig : (tv == ota::TrailerVerdict::Ok ? tr.der : nullptr);
  const uint8_t sigLen = _expect.sigLen ? _expect.sigLen : (tv == ota::TrailerVerdict::Ok ? tr.sigLen : 0);
  bool refuse = false;
  const char* why = signatureVerdict(sha, sig, sigLen, &refuse);
  if (refuse) return fail("%s", why);
  if (*why) {
    snprintf(_warning, sizeof(_warning), "%s", why);
    ESP_LOGW(TAG, "%s: installed anyway", why);
  }
  // Everything checked: the head goes in, and from here ota_0 may boot.
  const esp_err_t e = esp_partition_write(_slot, 0, _first, kHeadHold);
  if (e != ESP_OK) return fail("flash write at 0: 0x%x", unsigned(e));
  const esp_partition_pos_t pos = {_slot->address, _slot->size};
  esp_image_metadata_t md = {};
  if (esp_image_verify(ESP_IMAGE_VERIFY, &pos, &md) != ESP_OK) return fail("image check failed");  // abandon() erases it
  _touched = false;  // a whole, checked image: nothing to undo, a later begin() keeps it
  ESP_LOGI(TAG, "image %s: %u B written and checked", _head.version, unsigned(_image));
  return true;
}

void ImageWriter::abandon() {
  if (_touched && _slot) {
    // The head is in only when the very last check failed; else it is
    // still blank there and this is one erase more: nothing boots it.
    (void)esp_partition_erase_range(_slot, 0, ESPRack::FlashSteps::kSector);
  }
  _started = false;
  _touched = false;
  _hold.reset();
  _fill = 0;
}

bool ImageWriter::fail(const char* fmt, ...) {
  if (!_error[0]) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(_error, sizeof(_error), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "%s", _error);
  }
  return false;
}

}  // namespace loader
