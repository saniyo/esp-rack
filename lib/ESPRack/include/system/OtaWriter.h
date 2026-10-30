#pragma once
#ifndef ESPRACK_OTA_WRITER_H
#define ESPRACK_OTA_WRITER_H

// OtaWriter: a firmware image written into the next OTA slot in steps the
// write gate (power/WriteGate.h) takes one at a time. Replaces Arduino's
// Update for the framework's writers: Update erases a whole 64 KB block in
// one call at every 64 KB boundary (cache off ~0.15-1 s), which fits no
// pause between a module's windows.
//
//   OtaWriter ota;                                   (lives up to the restart)
//   { WriteSlot slot(...) only if OtaWriter::beginWrites() }
//   if (!ota.begin("auto-update", size, md5)) fail(ota.errorString());
//   for each step (<= 4 KB, FlashSteps::step):
//     WriteSlot slot; if (!slot.ok()) { ota.abort(); ... }   // PowerGuard
//     if (!ota.write(buf, n)) fail(...);
//   { WriteSlot slot only if OtaWriter::endWrites() } if (!ota.end()) fail(...);
//   ota.restartInto(ESPRack::kRestartAutoUpdate);   // sets it to boot, restarts
//
// From an UploadPump: ota.reserve("upload") first (the update is this
// writer's from the first chunk, while open may wait for a slot), then
// pump.begin(request, ota.sink("upload")), and ota.abort() if the pump
// refuses (the pump gives every call that writes flash its slot and ends
// a failed upload with the sink's abort, net/UploadPump.h); once the pump
// says Ok, ota.restartInto(...).
//
// What each call does to the flash (IDF 5.4, checked; 5.5 for begin):
//   begin     esp_ota_begin(OTA_WITH_SEQUENTIAL_WRITES): erases nothing,
//             writes nothing up to IDF 5.4. From IDF 5.5 with rollback
//             enabled it may erase the inactive otadata sector: then
//             beginWrites() is true and it goes in a slot. Fails while the
//             running image is still pending verification (rollback).
//   write     esp_ota_write: the first write into a sector erases it, so
//             a write of up to 4 KB erases one sector at most, and steps
//             ending on 4 KB boundaries erase exactly one each. Longer
//             writes are refused (several erases in one slot).
//   end       checks the MD5 (when given), then esp_ota_end: reads the
//             whole image to validate it (~0.1-0.3 s, the caches stay on);
//             writes a <= 16 B tail only with flash encryption: then
//             endWrites() is true and it goes in a slot, else no slot.
//   activate  esp_ota_set_boot_partition: reads and validates the whole
//             image once more (~0.1-0.3 s), then erases the otadata sector
//             and writes 32 B. The erase can't be placed in a pause
//             between a module's windows: restartInto() runs it in the
//             restart's afterShutdown step (App.h), after every module's
//             onShutdown (the buses stopped), in the slot that step has.
//             Without a restart nothing is set to boot.
//   abort     esp_ota_abort: frees the handle, no flash write.
// One image at a time on the device: begin() (or reserve()) takes the
// update for this writer; abort() and a failed call give it back. An
// ended image keeps it up to the restart (restartInto), so no other writer
// overwrites the slot about to boot. While another OtaWriter holds it, or
// another writer reports one (FirmwareUpdate::tryBegin: ArduinoOTA, a
// consumer's), begin() and reserve() fail (kErrBusy). The next slot is the
// same for every writer: two images at once would erase each other's
// sectors.
// FirmwareUpdate (system/FirmwareUpdate.h) is reported here: begin, each
// write, and end when failed or aborted (an ended image: at the restart).
// One user per writer (not thread-safe); ESP32 family only.

#include <stddef.h>
#include <stdint.h>

#include "net/UploadSink.h"
#include "system/FlashSteps.h"

namespace ESPRack {

class OtaWriter {
 public:
  static constexpr uint32_t kStep = FlashSteps::kSector;  // the longest write()
  // error() values of the writer's own (else an esp_err_t; ESP_FAIL is -1)
  static constexpr int kErrMd5 = -1001;      // the image's MD5 is not the one given
  static constexpr int kErrBadMd5 = -1002;   // the MD5 given is not 32 hex digits
  static constexpr int kErrTooBig = -1003;   // the image doesn't fit the slot
  static constexpr int kErrStep = -1004;     // a write longer than kStep
  static constexpr int kErrNotBegun = -1005;  // write/end/activate without begin
  static constexpr int kErrBusy = -1006;     // another firmware update is under way

  OtaWriter() = default;
  ~OtaWriter() { abort(); }
  OtaWriter(const OtaWriter&) = delete;
  OtaWriter& operator=(const OtaWriter&) = delete;

  // A new image in the next OTA slot. source: FirmwareUpdate's name (a
  // static string). sizeHint: the image's size if known (> the slot:
  // refused; 0: unknown). md5Hex: 32 hex digits to check it against, or
  // null. An image under way of this writer is dropped first (the update
  // stays this writer's).
  bool begin(const char* source, uint32_t sizeHint = 0, const char* md5Hex = nullptr);
  // The update for this writer ahead of begin() (e.g. from an upload's
  // first chunk, when begin() runs later on the FlashWriter). No flash
  // access. false: this writer already holds it, or another one does
  // (error() kErrBusy). abort() gives it back.
  bool reserve(const char* source);
  // In a slot. At most kStep bytes; erases one sector at most.
  bool write(const uint8_t* data, size_t len);
  // In a slot when endWrites(). Checks and closes the image; false: it is
  // no good (the writer is then idle, nothing to abort). The update stays
  // this writer's (restartInto, or abort to drop the image).
  bool end();
  // After end(): restarts the device (restartDevice) and sets the image to
  // boot in the restart's afterShutdown step (see above); this writer must
  // live up to the restart. Not ended: a plain restart. While PowerGuard
  // holds writes the image is not set to boot (logged): the running one
  // boots again.
  [[noreturn]] void restartInto(const char* reason);
  // After end(): esp_ota_set_boot_partition now. A full image read before
  // its otadata erase: only where no module's windows run (restartInto
  // calls it there).
  bool activate();
  // Discards the image under way or ended (none: nothing) and gives the
  // update back. No flash write.
  void abort();

  // UploadPump's sink: open = begin(source, sizeHint), write, finish =
  // end, abort. The calls that write no flash are marked so (no slot),
  // and it is revocable (an ended image boots only by restartInto).
  UploadSink sink(const char* source, uint32_t sizeHint = 0);

  bool active() const { return _handle != 0; }
  bool ended() const { return _ended; }        // end() succeeded, not activated yet
  uint32_t written() const { return _written; }
  uint32_t erases() const { return _erases; }  // sectors erased by this image's writes
  uint32_t slotSize() const { return _slotSize; }
  int error() const { return _error; }         // 0: none
  const char* errorString() const;

  // The next update slot's size (0: none, or only the running one: the
  // loader scheme), for a size check before begin().
  static uint32_t freeSlotSize();
  // begin() may erase (IDF >= 5.5 with rollback): take a slot for it.
  static bool beginWrites();
  // end() writes (flash encryption): take a slot for it.
  static bool endWrites();
  // An OtaWriter holds the update (reserved, an image under way, or ended
  // and waiting for the restart). Any task.
  static bool busy();

 private:
  bool fail(int err);
  bool claim(const char* source);
  void unclaim();

  uint32_t _handle = 0;  // esp_ota_handle_t
  const void* _part = nullptr;  // const esp_partition_t*
  const char* _source = nullptr;  // set: this writer holds the update
  bool _ended = false;
  bool _md5 = false;
  uint8_t _md5Want[16] = {};
  alignas(8) uint8_t _md5Ctx[96] = {};  // md5_context_t (esp_rom_md5.h), kept opaque here
  uint32_t _written = 0;
  uint32_t _erases = 0;
  uint32_t _slotSize = 0;
  int _error = 0;  // the first failure of this image
};

}  // namespace ESPRack

#endif  // ESPRACK_OTA_WRITER_H
