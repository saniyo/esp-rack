#pragma once
#ifndef ESPRACK_PURE_POSIX_TZ_H
#define ESPRACK_PURE_POSIX_TZ_H

// PosixTz — a POSIX TZ rule (the tzfile footer, RFC 8536 §3.3) evaluated
// without libc: MinGW/MSVCRT ignore `Mm.w.d` rules and newlib's TZ is global
// state. Pure, no heap, ~40 bytes.
//
//   std offset [dst [offset] [,start[/time],end[/time]]]
//   names: 3+ letters, or <...> with letters, digits, '+', '-'
//   offset: [+-]hh[:mm[:ss]], hh 0..24, POSITIVE WEST of Greenwich (POSIX)
//   rules: Mm.w.d (w 5 = last), Jn (1..365, Feb 29 never counted), n (0..365)
//   time: [+-]hhh[:mm[:ss]], -167..167 h (RFC 8536 extension), default 02:00
//   dst offset defaults to std + 1 h; dst without rules = M3.2.0,M11.1.0
//
// Offsets returned by this class are local - UTC in seconds (east positive).

#include <stdint.h>

namespace ESPRack {

struct CivilTime {
  int32_t y;
  uint8_t mo, d, h, mi, s, wdayMon0;  // wdayMon0 is ignored by toUtc()
};

class PosixTz {
 public:
  enum class Gap : uint8_t { AddGap, Reject };   // AddGap = convert with the pre-transition offset
  enum class Fold : uint8_t { Earlier, Later };  // which of the two instants a repeated local time means

  PosixTz() = default;  // UTC

  // false (and the zone becomes UTC) on a malformed string, nullptr, an empty
  // string or the ":path" form.
  bool parse(const char* posix);

  int32_t offsetAt(int64_t utcS) const { return isDst(utcS) ? _dst : _std; }
  int32_t stdOffset() const { return _std; }
  int32_t dstOffset() const { return _dst; }  // == stdOffset() without DST
  bool hasDst() const { return _hasDst; }
  bool isDst(int64_t utcS) const;

  CivilTime toLocal(int64_t utcS) const;
  // false: invalid fields (month, day of month, h > 23, mi/s > 59), or a
  // local time inside a gap with Gap::Reject.
  bool toUtc(const CivilTime& local, int64_t& outUtcS, Gap = Gap::AddGap, Fold = Fold::Earlier) const;

 private:
  struct Rule {
    uint8_t kind;  // 0 Mm.w.d, 1 Jn, 2 n
    uint8_t m, w, d;
    uint16_t n;
    int32_t timeS;
  };
  int32_t _std = 0, _dst = 0;
  bool _hasDst = false;
  Rule _start{0, 3, 2, 0, 0, 7200}, _end{0, 11, 1, 0, 0, 7200};

  static bool parseRule(const char*& p, Rule& r);
  static int64_t ruleDay(const Rule& r, int32_t year);  // days since 1970-01-01 (local date)
};

}  // namespace ESPRack

#endif  // ESPRACK_PURE_POSIX_TZ_H
