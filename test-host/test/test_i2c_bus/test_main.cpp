// Shared I2C bus (SES-LIB-034): the bus instance holds the controller and
// its pins, devices claim only their address on it (i2cAddrClaim), an
// old-style device next to a bus instance is a Conflict; I2cBusGuard and
// the result names.
#include <unity.h>

#include <stdio.h>
#include <string.h>

#include <hw/HwClaims.h>
#include <hw/II2cBus.h>

using namespace ESPRack;

void setUp(void) {}
void tearDown(void) {}

#define ASSERT_REASON(sub, c) \
  TEST_ASSERT_TRUE_MESSAGE(strstr((c).reason, (sub)) != nullptr, (c).reason)
#define ASSERT_OK(c) TEST_ASSERT_TRUE_MESSAGE((c).ok, (c).reason)
#define ASSERT_ERR(sub, c)      \
  do {                          \
    HwCheck r_ = (c);           \
    TEST_ASSERT_FALSE(r_.ok);   \
    ASSERT_REASON(sub, r_);     \
  } while (0)
#define ASSERT_STATE(exp, s) TEST_ASSERT_EQUAL_STRING(hwStateName(HwState::exp), hwStateName(s))

// A bus that only counts the lock calls; transfers aren't the core's.
class FakeBus : public II2cBus {
 public:
  explicit FakeBus(int controller) : _controller(controller) {}
  int controller() const override { return _controller; }
  bool ready() const override { return false; }
  I2cResult write(uint8_t, const uint8_t*, size_t) override { return I2cResult::NotReady; }
  I2cResult read(uint8_t, uint8_t*, size_t) override { return I2cResult::NotReady; }
  I2cResult writeRead(uint8_t, const uint8_t*, size_t, uint8_t*, size_t) override { return I2cResult::NotReady; }
  bool probe(uint8_t) override { return false; }
  bool lock(uint32_t waitMs) override {
    ++locks;
    lastWait = waitMs;
    if (!available) return false;
    ++depth;  // recursive: the same task may lock again
    return true;
  }
  void unlock() override {
    ++unlocks;
    --depth;
  }
  I2cBusStats stats() const override { return I2cBusStats{}; }

  bool available = true;  // false: another task holds it
  int depth = 0;
  int locks = 0, unlocks = 0;
  uint32_t lastWait = 0;

 private:
  int _controller;
};

static HwClaims claims(HwChip chip) { return HwClaims(boardInfoFor(chip, BoardFlags{})); }

// What the i2c-bus instance claims: the controller and both pins, exclusively.
static HwDescriptor busOn(int16_t controller, int16_t sda, int16_t scl) {
  HwDescriptor d;
  d.add(HwKind::I2cBus, controller, "bus");
  d.add(HwKind::Gpio, sda, "sda", kHwOutput);
  d.add(HwKind::Gpio, scl, "scl", kHwOutput);
  return d;
}

// A driver that opens Wire itself (before SES-LIB-034).
static HwDescriptor oldDevice(uint8_t addr, int8_t bus, int16_t sda, int16_t scl) {
  HwDescriptor d;
  d.add(HwKind::I2cBus, bus, "bus", kHwShared);
  d.add(HwKind::Gpio, sda, "sda", kHwShared, bus);
  d.add(HwKind::Gpio, scl, "scl", kHwShared, bus);
  d.add(HwKind::I2cAddr, addr, "addr", 0, bus);
  return d;
}

static HwDescriptor device(const II2cBus* bus, uint8_t addr, const char* label, uint8_t flags = 0) {
  HwDescriptor d;
  TEST_ASSERT_TRUE(i2cAddrClaim(bus, addr, label, d, flags));
  return d;
}

// HwRegistry::evaluate's rule (Disabled aside): the claims decide Conflict,
// then a missing required resource NotConfigured. Claims what an Ok
// instance holds, as the boot pass does.
static HwState apply(HwClaims& c, const char* owner, const HwDescriptor& d) {
  if (!c.check(owner, d).ok) return HwState::Conflict;
  if (d.missing()) return HwState::NotConfigured;
  c.set(owner, d);
  return HwState::Ok;
}

static void test_result_names(void) {
  TEST_ASSERT_EQUAL_STRING("ok", i2cResultName(I2cResult::Ok));
  TEST_ASSERT_EQUAL_STRING("nack", i2cResultName(I2cResult::Nack));
  TEST_ASSERT_EQUAL_STRING("timeout", i2cResultName(I2cResult::Timeout));
  TEST_ASSERT_EQUAL_STRING("busy", i2cResultName(I2cResult::Busy));
  TEST_ASSERT_EQUAL_STRING("not-ready", i2cResultName(I2cResult::NotReady));
  TEST_ASSERT_EQUAL_STRING("too-long", i2cResultName(I2cResult::TooLong));
  TEST_ASSERT_EQUAL_STRING("error", i2cResultName(I2cResult::Error));
  // every value has its own name, nothing past Error has one
  const int n = static_cast<int>(I2cResult::Error) + 1;
  for (int i = 0; i < n; ++i) {
    const char* a = i2cResultName(static_cast<I2cResult>(i));
    TEST_ASSERT_TRUE(strcmp(a, "?") != 0);
    for (int j = 0; j < i; ++j) TEST_ASSERT_TRUE(strcmp(a, i2cResultName(static_cast<I2cResult>(j))) != 0);
  }
  TEST_ASSERT_EQUAL_STRING("?", i2cResultName(static_cast<I2cResult>(n)));

  TEST_ASSERT_EQUAL_STRING("i2cBus.v1", II2cBus::kIfaceId);
  TEST_ASSERT_EQUAL_UINT(128, II2cBus::kMaxXfer);
  const I2cBusStats s{};
  TEST_ASSERT_EQUAL_UINT32(0, s.xfers + s.nacks + s.timeouts + s.busyWaits + s.recoveries + s.errors);
}

static void test_addr_claim(void) {
  FakeBus bus1(1);
  HwDescriptor d = device(&bus1, 0x50, "fram");
  TEST_ASSERT_EQUAL_UINT8(1, d.count);
  TEST_ASSERT_TRUE(d.res[0].kind == HwKind::I2cAddr);
  TEST_ASSERT_EQUAL_INT(0x50, d.res[0].index);
  TEST_ASSERT_EQUAL_INT(1, d.res[0].bus);
  TEST_ASSERT_EQUAL_STRING("fram", d.res[0].role);
  TEST_ASSERT_NULL(d.missing());

  HwDescriptor full;  // false when the descriptor is full, like add()
  for (uint8_t i = 0; i < HwDescriptor::kMax; ++i) full.add(HwKind::Gpio, i, "x");
  TEST_ASSERT_FALSE(i2cAddrClaim(&bus1, 0x50, "fram", full));
}

// The bus and two devices on it, on every chip (a pin pair each can use).
static void test_bus_and_devices(void) {
  const struct {
    HwChip chip;
    int16_t sda, scl;
  } boards[] = {{HwChip::Esp32, 21, 22}, {HwChip::Esp32S3, 8, 9}, {HwChip::Esp32C3, 8, 9}, {HwChip::Esp32C6, 6, 7}};
  for (const auto& b : boards) {
    HwClaims c = claims(b.chip);
    FakeBus bus0(0);
    ASSERT_STATE(Ok, apply(c, "i2c0", busOn(0, b.sda, b.scl)));
    ASSERT_STATE(Ok, apply(c, "rtc", device(&bus0, 0x68, "rtc")));
    ASSERT_STATE(Ok, apply(c, "fram", device(&bus0, 0x50, "fram")));
    TEST_ASSERT_EQUAL_UINT(5, c.size());  // bus, sda, scl, two addresses
    // the pins are the bus instance's alone
    for (int16_t pin : {b.sda, b.scl}) {
      char what[40];
      snprintf(what, sizeof(what), "GPIO%d used by i2c0 (%s)", pin, pin == b.sda ? "sda" : "scl");
      HwDescriptor led;
      led.add(HwKind::Gpio, pin, "led", kHwOutput);
      ASSERT_ERR(what, c.check("led", led));
    }
  }
}

static void test_same_address_conflict(void) {
  HwClaims c = claims(HwChip::Esp32C3);
  FakeBus bus0(0);
  ASSERT_STATE(Ok, apply(c, "i2c0", busOn(0, 8, 9)));
  ASSERT_STATE(Ok, apply(c, "rtc", device(&bus0, 0x68, "rtc")));
  ASSERT_ERR("I2C0 0x68 used by rtc (rtc)", c.check("rtc2", device(&bus0, 0x68, "rtc")));
  ASSERT_STATE(Conflict, apply(c, "rtc2", device(&bus0, 0x68, "rtc")));
  ASSERT_ERR("I2C address 0x03 is reserved", c.check("x", device(&bus0, 0x03, "x")));
  ASSERT_ERR("I2C address 0x80 is reserved", c.check("x", device(&bus0, 0x80, "x")));  // an 8-bit address
}

// A chip with two controllers: one bus instance each, the same address on
// both is fine. A chip with one: the second controller doesn't exist.
static void test_two_controllers(void) {
  for (HwChip chip : {HwChip::Esp32, HwChip::Esp32S3}) {
    HwClaims c = claims(chip);
    FakeBus bus0(0), bus1(1);
    ASSERT_STATE(Ok, apply(c, "i2c0", busOn(0, 13, 14)));
    ASSERT_STATE(Ok, apply(c, "i2c1", busOn(1, 4, 5)));
    ASSERT_STATE(Ok, apply(c, "rtc", device(&bus0, 0x68, "rtc")));
    ASSERT_STATE(Ok, apply(c, "rtc2", device(&bus1, 0x68, "rtc")));
    // a second bus instance on a taken controller
    ASSERT_ERR("I2C1 used by i2c1 (bus)", c.check("i2c1b", busOn(1, 16, 17)));
  }
  HwClaims c3 = claims(HwChip::Esp32C3);
  FakeBus bus1(1);
  ASSERT_ERR("I2C1 doesn't exist on ESP32-C3", c3.check("i2c1", busOn(1, 4, 5)));
  ASSERT_ERR("I2C1 doesn't exist on ESP32-C3", c3.check("rtc", device(&bus1, 0x68, "rtc")));
}

static void test_device_without_bus(void) {
  HwClaims c = claims(HwChip::Esp32S3);
  HwDescriptor d = device(nullptr, 0x68, "rtc");
  TEST_ASSERT_EQUAL_UINT8(1, d.count);
  TEST_ASSERT_TRUE(d.res[0].kind == HwKind::I2cAddr);
  TEST_ASSERT_EQUAL_INT(kHwNotConfigured, d.res[0].index);
  TEST_ASSERT_TRUE(d.missing() == &d.res[0]);  // HwRegistry: "rtc not set"
  TEST_ASSERT_EQUAL_STRING("rtc", d.missing()->role);
  ASSERT_OK(c.check("rtc", d));  // holds nothing, blocks nobody
  ASSERT_STATE(NotConfigured, apply(c, "rtc", d));
  ASSERT_STATE(NotConfigured, apply(c, "rtc2", device(nullptr, 0x68, "rtc")));
  TEST_ASSERT_EQUAL_UINT(0, c.size());

  // optional: the instance runs without it
  ASSERT_STATE(Ok, apply(c, "clock", device(nullptr, 0x68, "rtc", kHwOptional)));
  TEST_ASSERT_EQUAL_UINT(0, c.size());
}

// A driver that opens Wire itself can't share a controller with a bus
// instance: whichever comes second is the Conflict.
static void test_old_mode_conflict(void) {
  HwClaims c = claims(HwChip::Esp32S3);
  FakeBus bus0(0);
  ASSERT_STATE(Ok, apply(c, "i2c0", busOn(0, 8, 9)));
  ASSERT_STATE(Ok, apply(c, "fram", device(&bus0, 0x50, "fram")));
  ASSERT_ERR("I2C0 used by i2c0 (bus)", c.check("rtc", oldDevice(0x68, 0, 8, 9)));
  ASSERT_ERR("I2C0 used by i2c0 (bus)", c.check("rtc", oldDevice(0x68, 0, 4, 5)));  // other pins too
  ASSERT_ERR("GPIO8 used by i2c0 (sda)", c.check("rtc", oldDevice(0x68, 1, 8, 9)));  // its pins as I2C1
  ASSERT_OK(c.check("rtc", oldDevice(0x68, 1, 4, 5)));  // I2C1 on its own pins: fine
  ASSERT_STATE(Conflict, apply(c, "rtc", oldDevice(0x68, 0, 8, 9)));

  // the other way round: the old driver came first, the bus instance loses
  HwClaims o = claims(HwChip::Esp32S3);
  ASSERT_STATE(Ok, apply(o, "rtc", oldDevice(0x68, 0, 8, 9)));
  ASSERT_ERR("I2C0 used by rtc (bus)", o.check("i2c0", busOn(0, 8, 9)));
  HwDescriptor sda;  // the pins alone collide as well
  sda.add(HwKind::Gpio, 8, "sda", kHwOutput);
  ASSERT_ERR("GPIO8 used by rtc (sda)", o.check("i2c0", sda));
  // a device on the (Conflict) bus instance doesn't meet the old driver's address
  FakeBus lost(0);
  ASSERT_ERR("I2C0 0x68 used by rtc (addr)", o.check("rtc2", device(&lost, 0x68, "rtc")));
  ASSERT_OK(o.check("fram", device(&lost, 0x50, "fram")));
}

static void test_guard(void) {
  FakeBus bus(0);
  {
    I2cBusGuard g(&bus, 50);
    TEST_ASSERT_TRUE(g.ok());
    TEST_ASSERT_EQUAL_INT(1, bus.depth);
    TEST_ASSERT_EQUAL_UINT32(50, bus.lastWait);
    {
      I2cBusGuard inner(&bus, 10);  // the same task again: recursive
      TEST_ASSERT_TRUE(inner.ok());
      TEST_ASSERT_EQUAL_INT(2, bus.depth);
    }
    TEST_ASSERT_EQUAL_INT(1, bus.depth);
    TEST_ASSERT_EQUAL_INT(1, bus.unlocks);
  }
  TEST_ASSERT_EQUAL_INT(0, bus.depth);
  TEST_ASSERT_EQUAL_INT(2, bus.locks);
  TEST_ASSERT_EQUAL_INT(2, bus.unlocks);

  bus.available = false;  // held elsewhere: not taken, so not given back either
  {
    I2cBusGuard g(&bus, 5);
    TEST_ASSERT_FALSE(g.ok());
    TEST_ASSERT_EQUAL_INT(3, bus.locks);
  }
  TEST_ASSERT_EQUAL_INT(2, bus.unlocks);
  TEST_ASSERT_EQUAL_INT(0, bus.depth);

  // taken outside, refused inside: only the outer one unlocks
  bus.available = true;
  {
    I2cBusGuard outer(&bus, 5);
    bus.available = false;
    {
      I2cBusGuard inner(&bus, 5);
      TEST_ASSERT_FALSE(inner.ok());
    }
    TEST_ASSERT_EQUAL_INT(1, bus.depth);
  }
  TEST_ASSERT_EQUAL_INT(0, bus.depth);
  TEST_ASSERT_EQUAL_INT(3, bus.unlocks);

  I2cBusGuard none(nullptr, 5);  // no bus: nothing to lock
  TEST_ASSERT_FALSE(none.ok());
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_result_names);
  RUN_TEST(test_addr_claim);
  RUN_TEST(test_bus_and_devices);
  RUN_TEST(test_same_address_conflict);
  RUN_TEST(test_two_controllers);
  RUN_TEST(test_device_without_bus);
  RUN_TEST(test_old_mode_conflict);
  RUN_TEST(test_guard);
  return UNITY_END();
}
