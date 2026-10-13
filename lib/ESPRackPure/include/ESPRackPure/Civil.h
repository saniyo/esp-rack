#pragma once
#ifndef ESPRACK_PURE_CIVIL_H
#define ESPRACK_PURE_CIVIL_H

// Proleptic-Gregorian civil date <-> day count, UTC only (no time zones —
// see PosixTz). Algorithms: H. Hinnant, "chrono-Compatible Low-Level Date
// Algorithms". Day 0 = 1970-01-01. Valid for any int32 year.

#include <stdint.h>

namespace ESPRack {
namespace civil {

constexpr int64_t daysFromCivil(int32_t y, uint32_t m, uint32_t d) noexcept {
  const int64_t yy = static_cast<int64_t>(y) - (m <= 2 ? 1 : 0);
  const int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(yy - era * 400);              // [0, 399]
  const uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;   // [0, 365]
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;             // [0, 146096]
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int32_t& y, uint32_t& m, uint32_t& d) noexcept;

constexpr int64_t utcFromCivil(int32_t y, uint32_t mo, uint32_t d,
                               uint32_t h, uint32_t mi, uint32_t s) noexcept {
  return daysFromCivil(y, mo, d) * 86400 + static_cast<int64_t>(h) * 3600 +
         static_cast<int64_t>(mi) * 60 + static_cast<int64_t>(s);
}

constexpr bool isLeap(int32_t y) noexcept { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// 0 for a month outside 1..12.
constexpr uint8_t daysInMonth(int32_t y, uint32_t m) noexcept {
  return m == 2 ? (isLeap(y) ? 29 : 28)
                : (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : (m >= 1 && m <= 12) ? 31 : 0;
}

// Floor division (rounds toward -inf), b > 0.
constexpr int64_t floorDiv(int64_t a, int64_t b) noexcept {
  return a / b - ((a % b != 0 && a < 0) ? 1 : 0);
}

// 0 = Monday … 6 = Sunday. 1970-01-01 was a Thursday (3).
constexpr uint8_t weekdayMon0(int64_t days) noexcept {
  return static_cast<uint8_t>(((days + 3) % 7 + 7) % 7);
}

// UTC seconds of a __DATE__ string ("Mmm dd yyyy", day may be space-padded)
// at 00:00. 0 on malformed input. Useful as a "can't be earlier than the
// build" plausibility floor.
constexpr int64_t epochFromBuildDate(const char* date) noexcept {
  if (!date) return 0;
  const char* names = "JanFebMarAprMayJunJulAugSepOctNovDec";
  uint32_t mon = 0;
  for (uint32_t i = 0; i < 12; ++i) {
    if (date[0] == names[i * 3] && date[1] == names[i * 3 + 1] && date[2] == names[i * 3 + 2]) {
      mon = i + 1;
      break;
    }
  }
  if (mon == 0 || date[3] != ' ') return 0;
  uint32_t day = 0;
  for (int i = 4; i < 6; ++i) {
    const char c = date[i];
    if (c == ' ') continue;
    if (c < '0' || c > '9') return 0;
    day = day * 10 + static_cast<uint32_t>(c - '0');
  }
  if (date[6] != ' ' || day < 1 || day > 31) return 0;
  int32_t year = 0;
  for (int i = 7; i < 11; ++i) {
    const char c = date[i];
    if (c < '0' || c > '9') return 0;
    year = year * 10 + (c - '0');
  }
  return utcFromCivil(year, mon, day, 0, 0, 0);
}

}  // namespace civil
}  // namespace ESPRack

#endif  // ESPRACK_PURE_CIVIL_H
