#pragma once
#ifndef ESPRACK_PURE_LOCAL_CALENDAR_H
#define ESPRACK_PURE_LOCAL_CALENDAR_H

// Local calendar keys over a PosixTz. Keys are decimal and sort in time
// order: monthKey = YYYYMM, dateKey = YYYYMMDD (local, years 0..9999).

#include <stdint.h>

#include <ESPRackPure/PosixTz.h>

namespace ESPRack {
namespace cal {

uint32_t monthKey(const PosixTz& tz, int64_t utcS);
// [m0, m1) of the local month containing utcS: m0 = toUtc(local Y-M-01 00:00),
// m1 = the same for the next month (gap -> shifted forward, fold -> earlier).
void monthBounds(const PosixTz& tz, int64_t utcS, int64_t& m0, int64_t& m1);
uint32_t dateKey(const PosixTz& tz, int64_t utcS);
// The local date of the evening a night belongs to: dateKey(utcS - 12 h).
uint32_t eveningKey(const PosixTz& tz, int64_t utcS);
// UTC of local `dateKey` + secOfDay (may exceed 86400: rolls into the next
// days). Gap -> shifted forward; fold -> `fold`. kNoTime for an invalid key.
constexpr int64_t kNoTime = INT64_MIN;
int64_t atLocal(const PosixTz& tz, uint32_t dateKey, uint32_t secOfDay,
                PosixTz::Fold fold = PosixTz::Fold::Earlier);
uint8_t daysInMonth(int32_t y, uint8_t m);  // 0 for a month outside 1..12

}  // namespace cal
}  // namespace ESPRack

#endif  // ESPRACK_PURE_LOCAL_CALENDAR_H
