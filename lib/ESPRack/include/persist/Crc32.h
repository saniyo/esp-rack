#pragma once
#ifndef ESPRACK_PERSIST_CRC32_H
#define ESPRACK_PERSIST_CRC32_H

// CRC-32 (IEEE 802.3, reflected 0xEDB88320) with zlib-style chaining:
// crc32Ieee(b, nb, crc32Ieee(a, na)) is the CRC of a followed by b.
// Arduino-free (host-tested).

#include <stddef.h>
#include <stdint.h>

namespace ESPRack {

uint32_t crc32Ieee(const void* data, size_t len, uint32_t crc = 0);

}  // namespace ESPRack

#endif  // ESPRACK_PERSIST_CRC32_H
