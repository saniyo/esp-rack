#pragma once
#ifndef ESPRACK_LOADER_IMAGE_WRITER_H
#define ESPRACK_LOADER_IMAGE_WRITER_H

// ImageWriter: one image streamed into the app slot (ota_0), from a
// download or an upload alike.
//
//   ImageWriter& w = imageWriter();      (under Session::writer)
//   w.begin(slot, expect);
//   for each chunk: if (!w.feed(p, n)) fail(w.error());
//   if (!w.finish()) fail(w.error());    then esp_ota_set_boot_partition
//   on a failure: w.abandon()            (touched(): ota_0 no longer boots)
//
// The bytes go into a 4 KB buffer and reach the flash one whole sector at
// a time (FlashSteps): each step erases its one sector and writes it
// (esp_partition_*, as esp_ota_write does), nothing is erased up front.
// The first sector is checked before it is written (image header, chip,
// revision, not the loader): up to then ota_0 is untouched; that write is
// the point of no return (touched()). The image's first 16 bytes (its
// magic, one encryption block) stay in RAM and the flash keeps them
// erased; they go in last, once size, SHA-256 and signature passed (as
// Arduino's Update does), so whatever stops the writer before, a power
// cut included, leaves no image in ota_0 that boots: only a checked one
// ever does.
// The file's last 128 bytes are held back (OtaHandoff's TailHold): a
// signature trailer reaches neither the flash nor the hash. finish()
// checks size (sizeAsAnnounced: the image's or the signed file's) and
// SHA-256 against what was expected, the signature (the expected one,
// else the trailer's) against the compiled-in key, writes the head, then
// validates the image as the bootloader will (esp_image_verify, as
// esp_ota_end does).
// Signatures: with a production key a missing or bad one refuses the
// image; with a dev key it goes in with a warning().

#include <stddef.h>
#include <stdint.h>

#include <esp_partition.h>

#include "image_rules.h"
#include "sig_verify.h"
#include "system/OtaHandoff.h"

namespace loader {

class ImageWriter {
 public:
  struct Expect {
    uint32_t size = 0;                // the image's or the signed file's bytes, 0: any
    const uint8_t* sha256 = nullptr;  // 32 bytes, null: not given
    const uint8_t* sig = nullptr;     // DER, null: look for a trailer
    uint8_t sigLen = 0;
  };

  void begin(const esp_partition_t* slot, const Expect& expect);
  bool feed(const uint8_t* p, size_t n);
  bool finish();
  // Drops the image under way; once touched, erases ota_0's first sector
  // (one sector; the head may be in when only the last check failed). A
  // finished image is kept.
  void abandon();

  bool touched() const { return _touched; }  // written to, not finished
  uint32_t imageBytes() const { return _image; }
  const char* error() const { return _error; }
  const char* warning() const { return _warning; }
  const char* version() const { return _head.version; }

 private:
  bool take(const uint8_t* p, size_t n);  // image bytes
  bool flush();                           // the buffer's bytes into the flash
  bool fail(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

  static constexpr size_t kHeadHold = 16;  // the image's first bytes: written last

  const esp_partition_t* _slot = nullptr;
  Expect _expect;
  bool _started = false;  // the first sector passed its checks
  bool _touched = false;
  uint32_t _image = 0;  // image bytes taken (buffered or written)
  uint32_t _fill = 0;   // in _buf
  HeadInfo _head;
  ESPRack::ota::TailHold _hold;
  Sha256 _sha;
  char _error[64] = {};
  char _warning[96] = {};
  uint8_t _first[kHeadHold] = {};  // the image's first bytes (sector 0's write leaves them erased)
  alignas(4) uint8_t _buf[4096];
};

// The one writer (static storage): take Session::writer around it.
ImageWriter& imageWriter();

// The signature policy for a hash and a DER signature (sigLen 0: none):
// "" good; else why not, and *refuse says whether that refuses the image
// (production key) or is a warning (dev key).
const char* signatureVerdict(const uint8_t sha[32], const uint8_t* sig, uint8_t sigLen, bool* refuse);

}  // namespace loader

#endif  // ESPRACK_LOADER_IMAGE_WRITER_H
