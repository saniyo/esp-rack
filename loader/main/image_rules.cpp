#include "image_rules.h"

#include <string.h>

namespace loader {

namespace {

uint32_t le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// A fixed char field of the image: up to n chars, NUL-terminated in out.
void field(const uint8_t* p, size_t n, char* out) {
  size_t i = 0;
  for (; i < n - 1 && p[i]; ++i) out[i] = char(p[i]);
  out[i] = 0;
}

}  // namespace

const char* headText(Head h) {
  switch (h) {
    case Head::Ok:
      return "ok";
    case Head::TooShort:
      return "image too short";
    case Head::BadMagic:
      return "not an ESP app image";
    case Head::WrongChip:
      return "image built for another chip";
    case Head::NoAppDesc:
      return "image has no app description";
    case Head::IsLoader:
      return "image is the loader, not an app";
  }
  return "?";
}

Head checkHead(const uint8_t* p, size_t n, uint16_t chipId, HeadInfo& info) {
  if (n < kHeadNeeded) return Head::TooShort;
  if (p[0] != kImageMagic) return Head::BadMagic;
  info.chipId = uint16_t(p[12] | p[13] << 8);  // esp_image_header_t.chip_id
  if (info.chipId != chipId) return Head::WrongChip;
  if (le32(p + kAppDescOffset) != kAppDescMagic) return Head::NoAppDesc;
  field(p + kAppDescVersion, sizeof(info.version), info.version);
  field(p + kAppDescProject, sizeof(info.project), info.project);
  if (strcmp(info.project, kLoaderProject) == 0) return Head::IsLoader;
  return Head::Ok;
}

uint32_t retryPauseMs(uint16_t failed) {
  switch (failed) {
    case 0:
    case 1:
      return 10000;
    case 2:
      return 30000;
    case 3:
      return 120000;
    default:
      return 300000;
  }
}

bool hexEqual(const char* a, const char* b) {
  const size_t n = strlen(a);
  if (strlen(b) != n) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < n; ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'F') x = char(x - 'A' + 'a');
    if (y >= 'A' && y <= 'F') y = char(y - 'A' + 'a');
    diff |= uint8_t(x ^ y);
  }
  return diff == 0;
}

}  // namespace loader
