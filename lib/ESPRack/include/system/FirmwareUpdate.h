#pragma once
#ifndef ESPRACK_SYSTEM_FIRMWARE_UPDATE_H
#define ESPRACK_SYSTEM_FIRMWARE_UPDATE_H

// FirmwareUpdate: whether a firmware image is being written right now, so a
// supervisor (a watchdog module, a scheduled restart) doesn't reset the chip
// in the middle of it.
//
//   if (!ESPRack::FirmwareUpdate::active()) ESPRack::restartDevice("watchdog: ...");
//
// The writers report it: OtaWriter (system/OtaWriter.h) does for its users
// (auto-update, the upload page), ArduinoOTA too; a consumer that writes an
// image some other way calls begin() / progress() / end().
// active() also lapses when no progress came for ESPRACK_FW_UPDATE_STALL_MS:
// a client that vanished without an abort never blocks a restart for good.
// One image at a time: a writer that must not overlap another one takes
// the update with tryBegin() (false: another writer is active) and gives
// it back with end(source): only its own (a stale one may have been taken
// over by another writer meanwhile). source: the same pointer tryBegin()
// got (a static string).
// Any task.

#include <stdint.h>

#ifndef ESPRACK_FW_UPDATE_STALL_MS
#define ESPRACK_FW_UPDATE_STALL_MS 60000
#endif

namespace ESPRack {
namespace FirmwareUpdate {

void begin(const char* source);  // "upload", "ota", "auto-update", ...: a static string
bool tryBegin(const char* source);  // begin() unless another writer is active()
void progress();                 // each chunk written
void end();                      // finished or failed, whoever holds it; harmless when not begun
bool end(const char* source);    // end() if source holds it; false: another writer's (or none)
bool active();                   // begun, not ended, progress within the stall time
const char* source();            // the writer while active(), else ""

}  // namespace FirmwareUpdate
}  // namespace ESPRack

#endif  // ESPRACK_SYSTEM_FIRMWARE_UPDATE_H
