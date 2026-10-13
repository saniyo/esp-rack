// Minimal, self-contained Unity-compatible shim for host tests built by any
// C++17 compiler (MSVC included) — no esp-rack or Unity dependency. Drop-in
// for the subset of the Unity API the ESPRack/SES suites use, so the same
// test_main.cpp compiles against real Unity (`pio test -e native`, GCC hosts)
// and against this header (test-host/CMakeLists.txt).
//
// Differences from real Unity: a failed assertion throws (C++) to end the
// test; doubles are always supported; output format is Unity-like
// ("file:line:test:PASS|FAIL[: msg]" + "N Tests M Failures 0 Ignored").
//
// Usage (same as Unity):
//   #include <unity.h>
//   void setUp() {}  void tearDown() {}
//   void test_x() { TEST_ASSERT_EQUAL_INT64(4, f()); }
//   int main() { UNITY_BEGIN(); RUN_TEST(test_x); return UNITY_END(); }

#ifndef ESPRACK_UNITY_SHIM_H
#define ESPRACK_UNITY_SHIM_H

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

void setUp(void);
void tearDown(void);

namespace unity_shim {

struct Failure {
  std::string msg;
  int line;
};

struct State {
  const char* file = "";
  const char* test = "";
  int tests = 0;
  int failures = 0;
  int ignored = 0;
};

inline State& state() {
  static State s;
  return s;
}

[[noreturn]] inline void fail(int line, const std::string& msg) { throw Failure{msg, line}; }

inline std::string withMsg(const std::string& base, const char* msg) {
  return (msg && *msg) ? base + " " + msg : base;
}

template <typename T>
inline std::string num(T v) {
  return std::to_string(v);
}
inline std::string num(double v) {
  char b[64];
  std::snprintf(b, sizeof b, "%.9g", v);
  return b;
}

inline void assertInt(long long e, long long a, int line, const char* msg) {
  if (e != a) fail(line, withMsg("Expected " + num(e) + " Was " + num(a), msg));
}
inline void assertUint(unsigned long long e, unsigned long long a, int line, const char* msg) {
  if (e != a) fail(line, withMsg("Expected " + num(e) + " Was " + num(a), msg));
}
inline void assertIntWithin(long long d, long long e, long long a, int line, const char* msg) {
  const long long diff = e > a ? e - a : a - e;
  if (diff > d) fail(line, withMsg("Expected " + num(e) + " +/- " + num(d) + " Was " + num(a), msg));
}
inline void assertDoubleWithin(double d, double e, double a, int line, const char* msg) {
  if (!(std::fabs(e - a) <= d)) fail(line, withMsg("Expected " + num(e) + " +/- " + num(d) + " Was " + num(a), msg));
}
inline void assertStr(const char* e, const char* a, int line, const char* msg) {
  if (!e || !a || std::strcmp(e, a) != 0)
    fail(line, withMsg(std::string("Expected '") + (e ? e : "(null)") + "' Was '" + (a ? a : "(null)") + "'", msg));
}

inline void run(void (*fn)(void), const char* name, int line) {
  State& s = state();
  s.test = name;
  ++s.tests;
  try {
    setUp();
    fn();
    tearDown();
    std::printf("%s:%d:%s:PASS\n", s.file, line, name);
  } catch (const Failure& f) {
    ++s.failures;
    std::printf("%s:%d:%s:FAIL: %s\n", s.file, f.line, name, f.msg.c_str());
    try { tearDown(); } catch (...) {}
  }
}

inline int end() {
  State& s = state();
  std::printf("\n-----------------------\n%d Tests %d Failures %d Ignored\n%s\n", s.tests, s.failures, s.ignored,
              s.failures ? "FAIL" : "OK");
  return s.failures;
}

}  // namespace unity_shim

#define UNITY_BEGIN() (unity_shim::state().file = __FILE__, 0)
#define UNITY_END() unity_shim::end()
#define RUN_TEST(fn) unity_shim::run(fn, #fn, __LINE__)

#define TEST_FAIL_MESSAGE(m) unity_shim::fail(__LINE__, (m))
#define TEST_FAIL() unity_shim::fail(__LINE__, "Fail")
#define TEST_PASS() return
#define TEST_MESSAGE(m) std::printf("%s:%d:INFO: %s\n", __FILE__, __LINE__, (m))

#define TEST_ASSERT_TRUE_MESSAGE(c, m) do { if (!(c)) unity_shim::fail(__LINE__, std::string("Expected TRUE Was FALSE ") + ((m) ? (m) : "")); } while (0)
#define TEST_ASSERT_FALSE_MESSAGE(c, m) do { if ((c)) unity_shim::fail(__LINE__, std::string("Expected FALSE Was TRUE ") + ((m) ? (m) : "")); } while (0)
#define TEST_ASSERT_TRUE(c) TEST_ASSERT_TRUE_MESSAGE((c), "")
#define TEST_ASSERT_FALSE(c) TEST_ASSERT_FALSE_MESSAGE((c), "")
#define TEST_ASSERT(c) TEST_ASSERT_TRUE(c)
#define TEST_ASSERT_MESSAGE(c, m) TEST_ASSERT_TRUE_MESSAGE((c), (m))
#define TEST_ASSERT_NULL(p) TEST_ASSERT_TRUE((p) == nullptr)
#define TEST_ASSERT_NOT_NULL(p) TEST_ASSERT_TRUE((p) != nullptr)

#define TEST_ASSERT_EQUAL_INT_MESSAGE(e, a, m) unity_shim::assertInt((long long)(e), (long long)(a), __LINE__, (m))
#define TEST_ASSERT_EQUAL_INT(e, a) TEST_ASSERT_EQUAL_INT_MESSAGE((e), (a), "")
#define TEST_ASSERT_EQUAL(e, a) TEST_ASSERT_EQUAL_INT((e), (a))
#define TEST_ASSERT_EQUAL_MESSAGE(e, a, m) TEST_ASSERT_EQUAL_INT_MESSAGE((e), (a), (m))
#define TEST_ASSERT_EQUAL_INT8(e, a) TEST_ASSERT_EQUAL_INT((e), (a))
#define TEST_ASSERT_EQUAL_INT16(e, a) TEST_ASSERT_EQUAL_INT((e), (a))
#define TEST_ASSERT_EQUAL_INT32(e, a) TEST_ASSERT_EQUAL_INT((e), (a))
#define TEST_ASSERT_EQUAL_INT64(e, a) TEST_ASSERT_EQUAL_INT((e), (a))
#define TEST_ASSERT_EQUAL_INT64_MESSAGE(e, a, m) TEST_ASSERT_EQUAL_INT_MESSAGE((e), (a), (m))
#define TEST_ASSERT_EQUAL_INT32_MESSAGE(e, a, m) TEST_ASSERT_EQUAL_INT_MESSAGE((e), (a), (m))

#define TEST_ASSERT_EQUAL_UINT_MESSAGE(e, a, m) unity_shim::assertUint((unsigned long long)(e), (unsigned long long)(a), __LINE__, (m))
#define TEST_ASSERT_EQUAL_UINT(e, a) TEST_ASSERT_EQUAL_UINT_MESSAGE((e), (a), "")
#define TEST_ASSERT_EQUAL_UINT8(e, a) TEST_ASSERT_EQUAL_UINT((e), (a))
#define TEST_ASSERT_EQUAL_UINT16(e, a) TEST_ASSERT_EQUAL_UINT((e), (a))
#define TEST_ASSERT_EQUAL_UINT32(e, a) TEST_ASSERT_EQUAL_UINT((e), (a))
#define TEST_ASSERT_EQUAL_UINT64(e, a) TEST_ASSERT_EQUAL_UINT((e), (a))
#define TEST_ASSERT_EQUAL_UINT8_MESSAGE(e, a, m) TEST_ASSERT_EQUAL_UINT_MESSAGE((e), (a), (m))
#define TEST_ASSERT_EQUAL_UINT32_MESSAGE(e, a, m) TEST_ASSERT_EQUAL_UINT_MESSAGE((e), (a), (m))
#define TEST_ASSERT_EQUAL_UINT64_MESSAGE(e, a, m) TEST_ASSERT_EQUAL_UINT_MESSAGE((e), (a), (m))

#define TEST_ASSERT_INT_WITHIN_MESSAGE(d, e, a, m) unity_shim::assertIntWithin((long long)(d), (long long)(e), (long long)(a), __LINE__, (m))
#define TEST_ASSERT_INT_WITHIN(d, e, a) TEST_ASSERT_INT_WITHIN_MESSAGE((d), (e), (a), "")
#define TEST_ASSERT_INT64_WITHIN(d, e, a) TEST_ASSERT_INT_WITHIN((d), (e), (a))
#define TEST_ASSERT_INT64_WITHIN_MESSAGE(d, e, a, m) TEST_ASSERT_INT_WITHIN_MESSAGE((d), (e), (a), (m))

#define TEST_ASSERT_FLOAT_WITHIN_MESSAGE(d, e, a, m) unity_shim::assertDoubleWithin((double)(d), (double)(e), (double)(a), __LINE__, (m))
#define TEST_ASSERT_FLOAT_WITHIN(d, e, a) TEST_ASSERT_FLOAT_WITHIN_MESSAGE((d), (e), (a), "")
#define TEST_ASSERT_DOUBLE_WITHIN(d, e, a) TEST_ASSERT_FLOAT_WITHIN((d), (e), (a))
#define TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(d, e, a, m) TEST_ASSERT_FLOAT_WITHIN_MESSAGE((d), (e), (a), (m))

#define TEST_ASSERT_GREATER_THAN(t, a) TEST_ASSERT_TRUE_MESSAGE((a) > (t), "greater-than")
#define TEST_ASSERT_LESS_THAN(t, a) TEST_ASSERT_TRUE_MESSAGE((a) < (t), "less-than")
#define TEST_ASSERT_GREATER_OR_EQUAL(t, a) TEST_ASSERT_TRUE_MESSAGE((a) >= (t), "greater-or-equal")
#define TEST_ASSERT_LESS_OR_EQUAL(t, a) TEST_ASSERT_TRUE_MESSAGE((a) <= (t), "less-or-equal")

#define TEST_ASSERT_EQUAL_STRING_MESSAGE(e, a, m) unity_shim::assertStr((e), (a), __LINE__, (m))
#define TEST_ASSERT_EQUAL_STRING(e, a) TEST_ASSERT_EQUAL_STRING_MESSAGE((e), (a), "")

#endif  // ESPRACK_UNITY_SHIM_H
