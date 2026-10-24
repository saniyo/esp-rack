#pragma once
#ifndef ESPRACK_SYSTEM_FIRMWARE_UPDATE_H
#define ESPRACK_SYSTEM_FIRMWARE_UPDATE_H

// FirmwareUpdate: whether a firmware image is being written right now, so a
// supervisor (a watchdog module, a scheduled restart) doesn't reset the chip
// in the middle of it.
//
//   if (!ESPRack::FirmwareUpdate::active()) ESPRack::restartDevice("watchdog: ...");
//
// The writers report it: the upload page, ArduinoOTA and auto-update do;
// a consumer that writes an image itself calls begin() / progress() / end().
// active() also lapses when no progress came for ESPRACK_FW_UPDATE_STALL_MS:
// a client that vanished without an abort never blocks a restart for good.
// Any task.

#include <stdint.h>

#ifndef ESPRACK_FW_UPDATE_STALL_MS
#define ESPRACK_FW_UPDATE_STALL_MS 60000
#endif

namespace ESPRack {
namespace FirmwareUpdate {

void begin(const char* source);  // "upload", "ota", "auto-update", ...: a static string
void progress();                 // each chunk written
void end();                      // finished or failed; harmless when not begun
bool active();                   // begun, not ended, progress within the stall time
const char* source();            // the writer while active(), else ""

}  // namespace FirmwareUpdate
}  // namespace ESPRack

#endif  // ESPRACK_SYSTEM_FIRMWARE_UPDATE_H
