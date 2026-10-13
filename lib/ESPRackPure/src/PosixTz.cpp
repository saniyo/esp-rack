#include <ESPRackPure/PosixTz.h>

#include <ESPRackPure/Civil.h>

namespace ESPRack {

namespace {
constexpr uint8_t kRuleMonth = 0, kRuleJulian1 = 1, kRuleZero = 2;

inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

bool parseName(const char*& p) {
  const char* q = p;
  if (*q == '<') {
    const char* b = ++q;
    while (isAlpha(*q) || isDigit(*q) || *q == '+' || *q == '-') ++q;
    if (*q != '>' || q - b < 3) return false;
    p = q + 1;
    return true;
  }
  while (isAlpha(*q)) ++q;
  if (q - p < 3) return false;
  p = q;
  return true;
}

bool parseNum(const char*& p, int maxDigits, int32_t& out) {
  int32_t v = 0;
  int n = 0;
  while (isDigit(*p) && n < maxDigits) {
    v = v * 10 + (*p - '0');
    ++p;
    ++n;
  }
  if (n == 0 || isDigit(*p)) return false;
  out = v;
  return true;
}

// [+-]h[h[h]][:mm[:ss]] -> signed seconds
bool parseHms(const char*& p, int32_t maxH, int32_t& out) {
  int32_t sign = 1;
  if (*p == '+') {
    ++p;
  } else if (*p == '-') {
    sign = -1;
    ++p;
  }
  int32_t h = 0, m = 0, s = 0;
  if (!parseNum(p, 3, h) || h > maxH) return false;
  if (*p == ':') {
    ++p;
    if (!parseNum(p, 2, m) || m > 59) return false;
    if (*p == ':') {
      ++p;
      if (!parseNum(p, 2, s) || s > 59) return false;
    }
  }
  out = sign * (h * 3600 + m * 60 + s);
  return true;
}

inline int32_t yearOfDays(int64_t days) {
  int32_t y;
  uint32_t m, d;
  civil::civilFromDays(days, y, m, d);
  return y;
}
}  // namespace

bool PosixTz::parseRule(const char*& p, Rule& r) {
  int32_t v = 0;
  if (*p == 'M') {
    ++p;
    int32_t m = 0, w = 0, d = 0;
    if (!parseNum(p, 2, m) || m < 1 || m > 12 || *p != '.') return false;
    ++p;
    if (!parseNum(p, 1, w) || w < 1 || w > 5 || *p != '.') return false;
    ++p;
    if (!parseNum(p, 1, d) || d > 6) return false;
    r = Rule{kRuleMonth, static_cast<uint8_t>(m), static_cast<uint8_t>(w), static_cast<uint8_t>(d), 0, 7200};
  } else if (*p == 'J') {
    ++p;
    if (!parseNum(p, 3, v) || v < 1 || v > 365) return false;
    r = Rule{kRuleJulian1, 0, 0, 0, static_cast<uint16_t>(v), 7200};
  } else {
    if (!parseNum(p, 3, v) || v > 365) return false;
    r = Rule{kRuleZero, 0, 0, 0, static_cast<uint16_t>(v), 7200};
  }
  if (*p == '/') {
    ++p;
    if (!parseHms(p, 167, r.timeS)) return false;
  }
  return true;
}

bool PosixTz::parse(const char* posix) {
  *this = PosixTz{};
  if (!posix || *posix == ':') return false;
  const char* p = posix;
  int32_t off = 0;
  if (!parseName(p) || !parseHms(p, 24, off)) return false;
  PosixTz tz;
  tz._std = tz._dst = -off;
  if (*p != '\0') {
    if (!parseName(p)) return false;
    tz._hasDst = true;
    tz._dst = tz._std + 3600;
    if (*p != '\0' && *p != ',') {
      if (!parseHms(p, 24, off)) return false;
      tz._dst = -off;
    }
    if (*p == ',') {
      ++p;
      if (!parseRule(p, tz._start) || *p != ',') return false;
      ++p;
      if (!parseRule(p, tz._end)) return false;
    }
    if (*p != '\0') return false;
  }
  *this = tz;
  return true;
}

int64_t PosixTz::ruleDay(const Rule& r, int32_t year) {
  const int64_t jan1 = civil::daysFromCivil(year, 1, 1);
  if (r.kind == kRuleJulian1) return jan1 + r.n - 1 + ((civil::isLeap(year) && r.n >= 60) ? 1 : 0);
  if (r.kind == kRuleZero) return jan1 + r.n;
  const int64_t first = civil::daysFromCivil(year, r.m, 1);
  const uint8_t wdSun0 = static_cast<uint8_t>((civil::weekdayMon0(first) + 1) % 7);
  int64_t day = first + (r.d + 7 - wdSun0) % 7 + static_cast<int64_t>(r.w - 1) * 7;
  const int64_t next = first + civil::daysInMonth(year, r.m);
  while (day >= next) day -= 7;  // week 5 = the last such weekday
  return day;
}

bool PosixTz::isDst(int64_t utcS) const {
  if (!_hasDst) return false;
  // The rules of the year of the local standard time: start is in standard
  // time, end in DST (POSIX).
  const int32_t y = yearOfDays(civil::floorDiv(utcS + _std, 86400));
  const int64_t s = ruleDay(_start, y) * 86400 + _start.timeS - _std;
  const int64_t e = ruleDay(_end, y) * 86400 + _end.timeS - _dst;
  return s < e ? (utcS >= s && utcS < e) : !(utcS >= e && utcS < s);
}

CivilTime PosixTz::toLocal(int64_t utcS) const {
  const int64_t l = utcS + offsetAt(utcS);
  const int64_t days = civil::floorDiv(l, 86400);
  const int64_t sod = l - days * 86400;
  CivilTime t{};
  uint32_t m, d;
  civil::civilFromDays(days, t.y, m, d);
  t.mo = static_cast<uint8_t>(m);
  t.d = static_cast<uint8_t>(d);
  t.h = static_cast<uint8_t>(sod / 3600);
  t.mi = static_cast<uint8_t>(sod / 60 % 60);
  t.s = static_cast<uint8_t>(sod % 60);
  t.wdayMon0 = civil::weekdayMon0(days);
  return t;
}

bool PosixTz::toUtc(const CivilTime& lt, int64_t& outUtcS, Gap gap, Fold fold) const {
  if (lt.mo < 1 || lt.mo > 12 || lt.d < 1 || lt.d > civil::daysInMonth(lt.y, lt.mo) || lt.h > 23 ||
      lt.mi > 59 || lt.s > 59)
    return false;
  const int64_t l = civil::utcFromCivil(lt.y, lt.mo, lt.d, lt.h, lt.mi, lt.s);
  if (!_hasDst || _std == _dst) {
    outUtcS = l - _std;
    return true;
  }
  const int64_t uStd = l - _std, uDst = l - _dst;
  const bool okStd = !isDst(uStd), okDst = isDst(uDst);
  if (okStd && okDst) {  // fold: the local time occurs twice
    const int64_t lo = uStd < uDst ? uStd : uDst, hi = uStd < uDst ? uDst : uStd;
    outUtcS = fold == Fold::Earlier ? lo : hi;
    return true;
  }
  if (okStd || okDst) {
    outUtcS = okStd ? uStd : uDst;
    return true;
  }
  if (gap == Gap::Reject) return false;
  // gap: the offset grew from min to max at the transition; the pre-transition
  // offset lands the instant after it (the local time moves forward by the gap)
  outUtcS = l - (_std < _dst ? _std : _dst);
  return true;
}

}  // namespace ESPRack
