#pragma once
#ifndef ESPRACK_FLASH_STEPS_H
#define ESPRACK_FLASH_STEPS_H

// FlashSteps: a stream written into flash in steps the write gate
// (power/WriteGate.h) can take one at a time: one 4 KB sector erase at most
// per step.
//
// esp_ota_write in sequential mode (OTA_WITH_SEQUENTIAL_WRITES, IDF 5.4)
// erases a sector when a write first reaches it: a write of up to 4 KB
// erases one sector at most, 8 KB two, 64 KB on a 64 KB boundary one block
// with the cache off all along (no pause between a module's windows fits
// that). So a stream goes in steps of up to kSector, each one ending on a
// sector boundary: after the first (which may start mid-sector) every step
// is one whole sector and erases exactly that one. A file sink takes the
// same steps (LittleFS blocks are 4 KB too).
//
// Arduino-free (host-tested).

#include <stdint.h>

namespace ESPRack {
namespace FlashSteps {

constexpr uint32_t kSector = 4096;

// Bytes from offset to the next sector boundary (kSector on one).
inline uint32_t room(uint32_t offset) { return kSector - offset % kSector; }

// The next step of a stream at offset with avail bytes ready: up to the
// next sector boundary (0 when nothing is ready).
uint32_t step(uint32_t offset, uint32_t avail);

// The sectors a sequential write of len bytes at offset erases: those
// whose start lies inside it.
uint32_t erases(uint32_t offset, uint32_t len);

}  // namespace FlashSteps
}  // namespace ESPRack

#endif  // ESPRACK_FLASH_STEPS_H
