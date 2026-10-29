#include "system/FlashSteps.h"

namespace ESPRack {
namespace FlashSteps {

uint32_t step(uint32_t offset, uint32_t avail) {
  const uint32_t r = room(offset);
  return avail < r ? avail : r;
}

uint32_t erases(uint32_t offset, uint32_t len) {
  if (!len) return 0;
  // sector starts in [offset, offset + len): ceil(end / S) - ceil(offset / S)
  const uint64_t end = uint64_t(offset) + len;
  const uint64_t first = (uint64_t(offset) + kSector - 1) / kSector;
  const uint64_t last = (end + kSector - 1) / kSector;
  return static_cast<uint32_t>(last - first);
}

}  // namespace FlashSteps
}  // namespace ESPRack
