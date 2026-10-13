#include <ESPRackPure/TimeWindow.h>

#include <ESPRackPure/Civil.h>
#include <ESPRackPure/LocalCalendar.h>

namespace ESPRack {
namespace tw {

Interval intersect(Interval x, Interval y) {
  const int64_t lo = x.a > y.a ? x.a : y.a;
  const int64_t hi = x.b < y.b ? x.b : y.b;
  return hi > lo ? Interval{lo, hi} : Interval{lo, lo};
}

int64_t overlap(Interval x, Interval y) {
  const Interval r = intersect(x, y);
  return r.b - r.a;
}

bool forEvening(const PosixTz& tz, const DailyWindow& w, uint32_t eveningKey, Interval& out) {
  if (w.fromMin > 1439 || w.toMin > 1440) return false;
  const int32_t y = static_cast<int32_t>(eveningKey / 10000u);
  const uint32_t m = eveningKey / 100u % 100u, d = eveningKey % 100u;
  if (m < 1 || m > 12 || d < 1 || d > civil::daysInMonth(y, m)) return false;
  const uint8_t wd = civil::weekdayMon0(civil::daysFromCivil(y, m, d));
  if (!(w.dayMaskMon0 & (1u << wd))) return false;
  // minutes from the evening date's 00:00
  const uint32_t from = w.fromMin < 720 ? w.fromMin + 1440u : w.fromMin;
  const uint32_t to = from - w.fromMin + (w.toMin > w.fromMin ? w.toMin : w.toMin + 1440u);
  const int64_t a = cal::atLocal(tz, eveningKey, from * 60u, PosixTz::Fold::Earlier);
  const int64_t b = cal::atLocal(tz, eveningKey, to * 60u, PosixTz::Fold::Later);
  if (a == cal::kNoTime || b == cal::kNoTime || b <= a) return false;
  out = Interval{a, b};
  return true;
}

size_t clip(Interval* v, size_t n, int64_t a, int64_t b) {
  size_t k = 0;
  for (size_t i = 0; i < n; ++i) {
    const Interval r = intersect(v[i], Interval{a, b});
    if (r.b > r.a) v[k++] = r;
  }
  return k;
}

}  // namespace tw
}  // namespace ESPRack
