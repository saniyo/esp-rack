#include "persist/Crc32.h"

namespace ESPRack {

uint32_t crc32Ieee(const void* data, size_t len, uint32_t crc) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  crc = ~crc;
  while (len--) {
    crc ^= *p++;
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

}  // namespace ESPRack
