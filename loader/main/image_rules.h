#pragma once
#ifndef ESPRACK_LOADER_IMAGE_RULES_H
#define ESPRACK_LOADER_IMAGE_RULES_H

// The loader's checks that need no flash and no IDF: an app image's header
// before the first write (the point of no return), its size against the
// announced one and the retry pauses of a pull. The signature trailer and the records: system/OtaHandoff.h.
//
// ESP-IDF and Arduino-free (host-tested: loader/test-host).

#include <stddef.h>
#include <stdint.h>

#include "system/OtaHandoff.h"

namespace loader {

// ---- Image header ---------------------------------------------------------
// An app image starts with esp_image_header_t (24 B), the first segment's
// header (8 B) and esp_app_desc_t (esp_app_format.h): offsets fixed by the
// image format, the same on every chip.
constexpr size_t kHeaderLen = 24;
constexpr size_t kAppDescOffset = kHeaderLen + 8;
constexpr size_t kAppDescVersion = kAppDescOffset + 16;   // char version[32]
constexpr size_t kAppDescProject = kAppDescOffset + 48;   // char project_name[32]
constexpr size_t kHeadNeeded = kAppDescOffset + 80;      // through project_name
constexpr uint8_t kImageMagic = 0xE9;
constexpr uint32_t kAppDescMagic = 0xABCD5432u;
constexpr const char* kLoaderProject = "esprack-loader";  // this project's name

enum class Head : uint8_t {
  Ok,
  TooShort,   // fewer than kHeadNeeded bytes
  BadMagic,   // not an ESP image
  WrongChip,  // built for another chip
  NoAppDesc,  // no app description (not an app, or a bootloader / data)
  IsLoader    // the loader itself: never the app
};
const char* headText(Head h);

struct HeadInfo {
  uint16_t chipId = 0xFFFF;
  char version[32] = {};  // the image's app version (NUL-terminated)
  char project[32] = {};
};

// The image's first n bytes. chipId: the running chip's image chip ID
// (CONFIG_IDF_FIRMWARE_CHIP_ID). Revisions are checked on the target
// (bootloader_common_check_chip_validity).
Head checkHead(const uint8_t* p, size_t n, uint16_t chipId, HeadInfo& info);

// ---- Image size -----------------------------------------------------------
// A pull's announced size (Handoff::size) against the image that came: the
// image's bytes, or the whole signed file's (image + trailer: what a
// server's Content-Length says). announced 0: not given; trailer: the
// download ended in a good signature trailer.
inline bool sizeAsAnnounced(uint32_t announced, uint32_t imageLen, bool trailer) {
  return !announced || announced == imageLen || (trailer && announced == imageLen + ESPRack::ota::kTrailerSize);
}

// ---- Pull retries ---------------------------------------------------------
// After the point of no return a pull is never given up: 10 s, 30 s,
// 2 min, then every 5 min. failed: attempts failed so far (>= 1).
uint32_t retryPauseMs(uint16_t failed);
// The handoff's attempts (0: the default): past them the loader's page and
// log say the update is stuck; it goes on pulling all the same.
constexpr uint8_t kDefaultAttempts = 10;
inline uint16_t attemptsAllowed(uint8_t fromHandoff) { return fromHandoff ? fromHandoff : kDefaultAttempts; }

// Two hex strings equal ignoring case, compared in constant time over a's
// length (b must be as long).
bool hexEqual(const char* a, const char* b);

}  // namespace loader

#endif  // ESPRACK_LOADER_IMAGE_RULES_H
