// NvsRecord's blob codec: Arduino-free (host-tested).
#include <stddef.h>
#include <string.h>

#include "persist/NvsRecord.h"

namespace ESPRack {
namespace nvsrec {

namespace {
uint32_t crcOf(const Header& h, const void* payload, size_t len) {
  return crc32Ieee(payload, len, crc32Ieee(&h, offsetof(Header, crc)));
}
}  // namespace

void encode(uint8_t* out, uint16_t schema, uint32_t gen, const void* payload, uint16_t len) {
  Header h{kMagic, schema, len, gen, 0};
  h.crc = crcOf(h, payload, len);
  memcpy(out, &h, sizeof(h));
  memcpy(out + sizeof(h), payload, len);
}

Verdict decode(const uint8_t* blob, size_t size, uint16_t schema, uint16_t len, Header& h) {
  if (!blob || size < sizeof(Header)) return Verdict::Corrupt;
  memcpy(&h, blob, sizeof(h));
  if (h.magic != kMagic || size != sizeof(Header) + h.len) return Verdict::Corrupt;
  if (crcOf(h, blob + sizeof(Header), h.len) != h.crc) return Verdict::Corrupt;
  if (h.schema != schema) return Verdict::OtherSchema;
  return h.len == len ? Verdict::Ok : Verdict::Corrupt;
}

}  // namespace nvsrec
}  // namespace ESPRack
