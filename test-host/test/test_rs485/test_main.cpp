// Host tests of the RS485 / Modbus RTU frame state machine (hw/Rs485Framer).

#include <unity.h>

#include <string.h>

#include <hw/Rs485Framer.h>

using namespace ESPRack;
using namespace ESPRack::rs485;

void setUp(void) {}
void tearDown(void) {}

// The classic Modbus request: read 10 registers from 0, slave 1.
static const uint8_t kReq[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A, 0xC5, 0xCD};

static Rs485Config cfg(uint32_t baud, Parity p = Parity::None, uint8_t stop = 1) {
  Rs485Config c;
  c.baud = baud;
  c.parity = p;
  c.stopBits = stop;
  return c;
}

static void test_timing(void) {
  Timing t = timingFor(cfg(9600));
  TEST_ASSERT_EQUAL_UINT32(1042, t.charUs);  // 10 bits
  TEST_ASSERT_EQUAL_UINT32(1719, t.t15Us);   // 1.5 x 11 bits
  TEST_ASSERT_EQUAL_UINT32(4011, t.t35Us);
  TEST_ASSERT_EQUAL_UINT32(573, timingFor(cfg(19200, Parity::Even)).charUs);  // 11 bits
  t = timingFor(cfg(115200));
  TEST_ASSERT_EQUAL_UINT32(750, t.t15Us);    // fixed above 19200
  TEST_ASSERT_EQUAL_UINT32(1750, t.t35Us);
  Rs485Config c = cfg(9600);
  c.t35Us = 5000;
  TEST_ASSERT_EQUAL_UINT32(5000, timingFor(c).t35Us);
}

static void test_crc(void) {
  TEST_ASSERT_EQUAL_UINT16(0xCDC5, crc16(kReq, 6));
  TEST_ASSERT_TRUE(crcOk(kReq, sizeof kReq));
  uint8_t bad[sizeof kReq];
  memcpy(bad, kReq, sizeof kReq);
  bad[3] ^= 1;
  TEST_ASSERT_FALSE(crcOk(bad, sizeof bad));
}

// Bytes back to back from t0, one char apart; returns the last byte's time.
static int64_t feed(Framer& f, const uint8_t* d, size_t n, int64_t t0, Rs485Frame& out, int* closed) {
  const int64_t ch = f.timing().charUs;
  int64_t t = t0;
  for (size_t i = 0; i < n; ++i) {
    t = t0 + int64_t(i + 1) * ch;
    if (f.onByte(d[i], t, 0, out) && closed) ++*closed;
  }
  return t;
}

static void test_frame_closes_after_t35(void) {
  Framer f;
  f.reset(timingFor(cfg(9600)));
  Rs485Frame out{};
  int closed = 0;
  const int64_t last = feed(f, kReq, sizeof kReq, 1000000, out, &closed);
  TEST_ASSERT_EQUAL(0, closed);
  TEST_ASSERT_TRUE(f.open());
  TEST_ASSERT_FALSE(f.poll(last + 4000, out));  // t3.5 = 4011 µs
  TEST_ASSERT_TRUE(f.poll(last + 4011, out));
  TEST_ASSERT_EQUAL_UINT16(sizeof kReq, out.len);
  TEST_ASSERT_EQUAL_UINT16(kRxCrcOk, out.flags);
  TEST_ASSERT_EQUAL_INT64(1000000 + 1042, out.tFirstUs);
  TEST_ASSERT_EQUAL_INT64(last, out.tLastUs);
  TEST_ASSERT_FALSE(f.open());
}

static void test_gap_inside_frame(void) {
  Framer f;
  f.reset(timingFor(cfg(9600)));
  Rs485Frame out{};
  int64_t t = feed(f, kReq, 4, 0, out, nullptr);
  // 2 ms of silence: more than t1.5 (1719), less than t3.5 (4011).
  t += 1042 + 2000;
  TEST_ASSERT_FALSE(f.onByte(kReq[4], t, 0, out));
  for (size_t i = 5; i < sizeof kReq; ++i) f.onByte(kReq[i], t += 1042, 0, out);
  TEST_ASSERT_TRUE(f.poll(t + 5000, out));
  TEST_ASSERT_EQUAL_UINT16(sizeof kReq, out.len);
  TEST_ASSERT_EQUAL_UINT16(kRxCrcOk | kRxGapT15, out.flags);
}

static void test_silence_starts_a_new_frame(void) {
  Framer f;
  f.reset(timingFor(cfg(9600)));
  Rs485Frame out{};
  int closed = 0;
  int64_t t = feed(f, kReq, sizeof kReq, 0, out, &closed);
  // The next byte comes after 5 ms of silence: the first frame closes first.
  TEST_ASSERT_TRUE(f.onByte(0x01, t + 1042 + 5000, 0, out));
  TEST_ASSERT_EQUAL_UINT16(sizeof kReq, out.len);
  TEST_ASSERT_TRUE(out.flags & kRxCrcOk);
  TEST_ASSERT_TRUE(f.open());
}

static void test_junk_prefix_and_overrun(void) {
  Framer f;
  f.reset(timingFor(cfg(19200)));
  Rs485Frame out{};
  uint8_t junk[sizeof kReq + 1];
  junk[0] = 0xFF;
  memcpy(junk + 1, kReq, sizeof kReq);
  int64_t t = feed(f, junk, sizeof junk, 0, out, nullptr);
  TEST_ASSERT_TRUE(f.poll(t + 3000, out));
  TEST_ASSERT_EQUAL_UINT16(kRxJunkPrefix, out.flags);  // not crc ok as received

  // Lost bytes are flagged; more than 256 bytes are cut and flagged.
  t += 10000;
  TEST_ASSERT_FALSE(f.onByte(0x01, t, kRxOverrun, out));
  for (int i = 0; i < 300; ++i) f.onByte(0x55, t += 573, 0, out);
  TEST_ASSERT_TRUE(f.poll(t + 3000, out));
  TEST_ASSERT_EQUAL_UINT16(256, out.len);
  TEST_ASSERT_TRUE(out.flags & kRxOverrun);
  TEST_ASSERT_EQUAL_INT64(0, f.deadlineUs());
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_timing);
  RUN_TEST(test_crc);
  RUN_TEST(test_frame_closes_after_t35);
  RUN_TEST(test_gap_inside_frame);
  RUN_TEST(test_silence_starts_a_new_frame);
  RUN_TEST(test_junk_prefix_and_overrun);
  return UNITY_END();
}
