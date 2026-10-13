#include <ESPRackPure/LocalCalendar.h>

#include <ESPRackPure/Civil.h>

namespace ESPRack {
namespace cal {

namespace {
int64_t localMidnight(const PosixTz& tz, int32_t y, uint8_t m, uint8_t d) {
  int64_t out = 0;
  tz.toUtc(CivilTime{y, m, d, 0, 0, 0, 0}, out, PosixTz::Gap::AddGap, PosixTz::Fold::Earlier);
  return out;
}
}  // namespace

uint8_t daysInMonth(int32_t y, uint8_t m) { return civil::daysInMonth(y, m); }

uint32_t monthKey(const PosixTz& tz, int64_t utcS) {
  const CivilTime t = tz.toLocal(utcS);
  return static_cast<uint32_t>(t.y) * 100u + t.mo;
}

uint32_t dateKey(const PosixTz& tz, int64_t utcS) {
  const CivilTime t = tz.toLocal(utcS);
  return static_cast<uint32_t>(t.y) * 10000u + t.mo * 100u + t.d;
}

uint32_t eveningKey(const PosixTz& tz, int64_t utcS) { return dateKey(tz, utcS - 12 * 3600); }

void monthBounds(const PosixTz& tz, int64_t utcS, int64_t& m0, int64_t& m1) {
  const CivilTime t = tz.toLocal(utcS);
  m0 = localMidnight(tz, t.y, t.mo, 1);
  m1 = t.mo == 12 ? localMidnight(tz, t.y + 1, 1, 1)
                  : localMidnight(tz, t.y, static_cast<uint8_t>(t.mo + 1), 1);
}

int64_t atLocal(const PosixTz& tz, uint32_t key, uint32_t secOfDay, PosixTz::Fold fold) {
  const int32_t y = static_cast<int32_t>(key / 10000u);
  const uint32_t m = key / 100u % 100u, d = key % 100u;
  if (m < 1 || m > 12 || d < 1 || d > civil::daysInMonth(y, m)) return kNoTime;
  const int64_t days = civil::daysFromCivil(y, m, d) + secOfDay / 86400u;
  const uint32_t sod = secOfDay % 86400u;
  int32_t yy;
  uint32_t mm, dd;
  civil::civilFromDays(days, yy, mm, dd);
  const CivilTime lt{yy,
                     static_cast<uint8_t>(mm),
                     static_cast<uint8_t>(dd),
                     static_cast<uint8_t>(sod / 3600u),
                     static_cast<uint8_t>(sod / 60u % 60u),
                     static_cast<uint8_t>(sod % 60u),
                     0};
  int64_t out = 0;
  tz.toUtc(lt, out, PosixTz::Gap::AddGap, fold);
  return out;
}

}  // namespace cal
}  // namespace ESPRack
