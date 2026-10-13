#include <ESPRackPure/Civil.h>

namespace ESPRack {
namespace civil {

void civilFromDays(int64_t z, int32_t& y, uint32_t& m, uint32_t& d) noexcept {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(z - era * 146097);                     // [0, 146096]
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;      // [0, 399]
  const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                    // [0, 365]
  const uint32_t mp = (5 * doy + 2) / 153;                                          // [0, 11]
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int32_t>(yy + (m <= 2 ? 1 : 0));
}

}  // namespace civil
}  // namespace ESPRack
