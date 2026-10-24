#pragma once
#ifndef WEB_INT_COERCE_H
#define WEB_INT_COERCE_H

// Integer parse-back rules for FormBuilder::updateValue (SES-LIB-016).
// Arduino-free (host-tested in test-host/test/test_int_coerce).
//
// A form writes a number field back as a JSON integer, a JSON double, or a
// numeric string ("42", " -7 "). Every integer type follows the same rules:
//   * double  -> rounded half away from zero, clamped to the type's range
//   * string  -> optionally signed decimal integer, clamped; anything else
//                is not a value (the field keeps its value)
// On ESP32 (esp-14.2.0) int32_t/uint32_t are long/unsigned long, so these
// rules are what int32_t settings get.
//
// Floating-point settings (and uint8_t/uint16_t/int, which round a double)
// take a JSON number or a numeric string ("9.00", " -1.5 "). An empty or
// non-numeric string is not a value either: a cleared form field must not
// become 0 (and then, clamped, the minimum).

#include <errno.h>
#include <stdlib.h>

#include <limits>
#include <type_traits>

namespace web {
namespace coerce {

template <typename I>
I fromDouble(double d) {
  static_assert(std::is_integral<I>::value, "integer type");
  // NaN: the type's zero (no meaningful value). Bounds as doubles: the max of
  // a 64-bit type rounds up to 2^63/2^64, so compare with >= and saturate.
  if (d != d) return I(0);
  const double lo = static_cast<double>(std::numeric_limits<I>::min());
  const double hi = static_cast<double>(std::numeric_limits<I>::max());
  if (d <= lo) return std::numeric_limits<I>::min();
  if (d >= hi) return std::numeric_limits<I>::max();
  const double r = d >= 0 ? d + 0.5 : d - 0.5;
  if (r <= lo) return std::numeric_limits<I>::min();
  if (r >= hi) return std::numeric_limits<I>::max();
  return static_cast<I>(r);
}

// true + out = the clamped value when s holds an optionally signed decimal
// integer, with optional surrounding spaces; false otherwise.
template <typename I>
bool fromString(const char* s, I& out) {
  static_assert(std::is_integral<I>::value, "integer type");
  if (!s) return false;
  while (*s == ' ' || *s == '\t') ++s;
  const char* p = s;
  bool neg = false;
  if (*p == '+' || *p == '-') neg = (*p++ == '-');
  if (*p < '0' || *p > '9') return false;
  char* end = nullptr;
  errno = 0;
  if (neg) {
    const long long v = strtoll(s, &end, 10);
    const bool under = errno == ERANGE;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end) return false;
    if (std::is_unsigned<I>::value) {
      out = I(0);
    } else if (under || v < static_cast<long long>(std::numeric_limits<I>::min())) {
      out = std::numeric_limits<I>::min();
    } else {
      out = static_cast<I>(v);
    }
    return true;
  }
  const unsigned long long v = strtoull(p, &end, 10);
  const bool over = errno == ERANGE;
  while (*end == ' ' || *end == '\t') ++end;
  if (*end) return false;
  if (over || v > static_cast<unsigned long long>(std::numeric_limits<I>::max())) {
    out = std::numeric_limits<I>::max();
  } else {
    out = static_cast<I>(v);
  }
  return true;
}

// A decimal number in a string ("9.00", " -1.5 ", "1e3"), surrounding blanks
// allowed. false: empty, null, or anything that is not entirely a number.
// Hand-rolled: strtod would pull several KB of newlib into every firmware.
inline bool numberFromString(const char* s, double& out) {
  if (!s) return false;
  while (*s == ' ' || *s == '\t') ++s;
  bool neg = false;
  if (*s == '+' || *s == '-') neg = (*s++ == '-');
  unsigned long long mant = 0;
  int exp10 = 0;
  bool digits = false;
  for (; *s >= '0' && *s <= '9'; ++s, digits = true) {
    if (mant < 100000000000000000ULL) mant = mant * 10 + unsigned(*s - '0');
    else ++exp10;  // past 17 digits: keep the magnitude only
  }
  if (*s == '.') {
    for (++s; *s >= '0' && *s <= '9'; ++s, digits = true) {
      if (mant < 100000000000000000ULL) {
        mant = mant * 10 + unsigned(*s - '0');
        --exp10;
      }
    }
  }
  if (!digits) return false;
  if (*s == 'e' || *s == 'E') {
    ++s;
    bool eneg = false;
    if (*s == '+' || *s == '-') eneg = (*s++ == '-');
    if (*s < '0' || *s > '9') return false;
    int e = 0;
    for (; *s >= '0' && *s <= '9'; ++s) {
      if (e < 1000) e = e * 10 + (*s - '0');
    }
    exp10 += eneg ? -e : e;
  }
  while (*s == ' ' || *s == '\t') ++s;
  if (*s) return false;
  double v = static_cast<double>(mant);
  // One division (or multiplication) by a power of ten keeps "3.3" at 3.3.
  double p = 1.0;
  for (int i = exp10 < 0 ? -exp10 : exp10; i > 0 && p < 1e308; --i) p *= 10.0;
  v = exp10 < 0 ? v / p : v * p;
  out = neg ? -v : v;
  return true;
}

// A JSON integer that came in as a (wider) signed or unsigned integer.
template <typename I>
I fromSigned(long long v) {
  if (std::is_unsigned<I>::value && v < 0) return I(0);
  if (!std::is_unsigned<I>::value && v < static_cast<long long>(std::numeric_limits<I>::min())) {
    return std::numeric_limits<I>::min();
  }
  if (v >= 0 && static_cast<unsigned long long>(v) > static_cast<unsigned long long>(std::numeric_limits<I>::max())) {
    return std::numeric_limits<I>::max();
  }
  return static_cast<I>(v);
}

template <typename I>
I fromUnsigned(unsigned long long v) {
  if (v > static_cast<unsigned long long>(std::numeric_limits<I>::max())) return std::numeric_limits<I>::max();
  return static_cast<I>(v);
}

}  // namespace coerce
}  // namespace web

#endif  // WEB_INT_COERCE_H
