#pragma once
#ifndef ESPRACK_PURE_TIME_WINDOW_H
#define ESPRACK_PURE_TIME_WINDOW_H

// Half-open UTC intervals and local daily windows ("18:00-06:00, Mon-Fri")
// resolved for one evening. Pure, no heap.

#include <stddef.h>
#include <stdint.h>

#include <ESPRackPure/PosixTz.h>

namespace ESPRack {
namespace tw {

struct Interval {
  int64_t a, b;  // [a, b), UTC seconds; empty when b <= a
};

// Empty result: {lo, lo} with lo = max(x.a, y.a).
Interval intersect(Interval x, Interval y);
int64_t overlap(Interval x, Interval y);  // length of the intersection, >= 0

// A local daily window, minutes of the day. toMin <= fromMin wraps past
// midnight (fromMin == toMin is a full 24 h); toMin may be 1440 (24:00).
// dayMaskMon0: bit 0 = Monday … bit 6 = Sunday.
struct DailyWindow {
  uint16_t fromMin, toMin;
  uint8_t dayMaskMon0;
};

// The window instance that belongs to the night starting on the evening of
// local date `eveningKey` (YYYYMMDD, cal::eveningKey):
//   fromMin >= 12:00 -> starts on that date; fromMin < 12:00 -> starts on the
//   next date (an after-midnight slot belongs to the previous evening).
//   The day mask is tested on the evening date, never on the after-midnight one.
//   from converts with Fold::Earlier, to with Fold::Later; a gap is crossed
//   with the pre-transition offset (the endpoint moves forward by the gap).
// false: mask excludes the evening, fromMin > 1439, toMin > 1440, or an
// invalid eveningKey.
bool forEvening(const PosixTz& tz, const DailyWindow& w, uint32_t eveningKey, Interval& out);

// Clips every interval to [a, b) in place, drops the empty ones, keeps order.
// Returns the new count.
size_t clip(Interval* v, size_t n, int64_t a, int64_t b);

}  // namespace tw
}  // namespace ESPRack

#endif  // ESPRACK_PURE_TIME_WINDOW_H
