// HwClaims (LIB-FEAT-001): per-chip pin tables, reserved/caution pins,
// conflicts between instances, peripheral budgets, shared buses.
#include <unity.h>

#include <string.h>

#include <hw/BoardTables.h>
#include <hw/HwClaims.h>

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

static HwDescriptor one(HwKind k, int16_t idx, const char* role, uint8_t flags = 0, int8_t bus = -1) {
  HwDescriptor d;
  d.add(k, idx, role, flags, bus);
  return d;
}
static HwClaims claims(HwChip chip, BoardFlags f = BoardFlags{}) { return HwClaims(boardInfoFor(chip, f)); }

static void test_names(void) {
  char buf[24];
  TEST_ASSERT_EQUAL_STRING("GPIO17", hwResourceText(HwResource{HwKind::Gpio, 17, -1, 0, "tx"}, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("UART1", hwResourceText(HwResource{HwKind::Uart, 1, -1, 0, nullptr}, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("I2C0 0x68",
                           hwResourceText(HwResource{HwKind::I2cAddr, 0x68, 0, 0, nullptr}, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("rmt-tx any",
                           hwResourceText(HwResource{HwKind::RmtTx, kHwAnyChannel, -1, 0, nullptr}, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("gpio -", hwResourceText(HwResource{HwKind::Gpio, -1, -1, 0, nullptr}, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("Conflict", hwStateName(HwState::Conflict));
  TEST_ASSERT_EQUAL_STRING("pcnt", hwKindName(HwKind::PcntUnit));
}

static void test_board_tables(void) {
  BoardFlags f;
  BoardInfo e = boardInfoFor(HwChip::Esp32, f);
  TEST_ASSERT_TRUE(e.exists(39));
  TEST_ASSERT_FALSE(e.exists(24));
  TEST_ASSERT_FALSE(e.exists(40));
  TEST_ASSERT_FALSE(e.canOutput(34));
  TEST_ASSERT_TRUE(e.canOutput(33));
  TEST_ASSERT_TRUE(e.isAdc2(25));
  TEST_ASSERT_FALSE(e.isAdc2(32));

  BoardInfo s3 = boardInfoFor(HwChip::Esp32S3, f);
  TEST_ASSERT_TRUE(s3.exists(48));
  TEST_ASSERT_FALSE(s3.exists(22));
  TEST_ASSERT_FALSE(s3.exists(49));
  TEST_ASSERT_TRUE(s3.isAdc2(11));

  BoardInfo c3 = boardInfoFor(HwChip::Esp32C3, f);
  TEST_ASSERT_TRUE(c3.exists(21));
  TEST_ASSERT_FALSE(c3.exists(22));
  TEST_ASSERT_EQUAL_UINT8(0, c3.pcnt);

  BoardInfo c6 = boardInfoFor(HwChip::Esp32C6, f);
  TEST_ASSERT_TRUE(c6.exists(30));
  TEST_ASSERT_FALSE(c6.exists(31));
  TEST_ASSERT_EQUAL_UINT64(0, c6.adc2Mask);

  // The strongest note wins: Reserved over Caution
  BoardInfo b;
  b.addNote(5, PinLevel::Caution, "c");
  b.addNote(5, PinLevel::Reserved, "r");
  TEST_ASSERT_TRUE(b.note(5)->level == PinLevel::Reserved);
  TEST_ASSERT_NULL(b.note(6));
}

static void test_flash_and_console_reserved(void) {
  HwClaims e = claims(HwChip::Esp32);
  ASSERT_ERR("GPIO6 reserved: SPI flash", e.check("a", one(HwKind::Gpio, 6, "x")));
  ASSERT_ERR("UART0 console", e.check("a", one(HwKind::Gpio, 1, "x")));
  ASSERT_ERR("UART0 reserved", e.check("a", one(HwKind::Uart, 0, "port")));
  ASSERT_OK(e.check("a", one(HwKind::Uart, 2, "port")));
  ASSERT_ERR("doesn't exist", e.check("a", one(HwKind::Uart, 3, "port")));
  ASSERT_OK(e.check("a", one(HwKind::Gpio, 16, "x")));  // no PSRAM: free

  BoardFlags p;
  p.psram = true;
  HwClaims ep = claims(HwChip::Esp32, p);
  ASSERT_ERR("PSRAM", ep.check("a", one(HwKind::Gpio, 16, "x")));

  BoardFlags noCon;
  noCon.uart0Console = false;
  HwClaims en = claims(HwChip::Esp32, noCon);
  HwCheck c = en.check("a", one(HwKind::Gpio, 1, "x"));
  ASSERT_OK(c);
  TEST_ASSERT_TRUE(c.warning);
  ASSERT_REASON("ROM boot log", c);
  ASSERT_OK(en.check("a", one(HwKind::Uart, 0, "port")));
}

static void test_s3_octal_usb(void) {
  BoardFlags f;
  f.psram = true;
  f.octalPsramOrFlash = true;
  f.usbInUse = true;
  HwClaims s3 = claims(HwChip::Esp32S3, f);
  ASSERT_ERR("octal PSRAM/flash", s3.check("a", one(HwKind::Gpio, 35, "x")));
  ASSERT_ERR("PSRAM CS", s3.check("a", one(HwKind::Gpio, 26, "x")));
  ASSERT_ERR("USB", s3.check("a", one(HwKind::Gpio, 19, "x")));
  ASSERT_ERR("GPIO22 doesn't exist on ESP32-S3", s3.check("a", one(HwKind::Gpio, 22, "x")));
  ASSERT_OK(s3.check("a", one(HwKind::Gpio, 38, "x")));

  HwClaims plain = claims(HwChip::Esp32S3);  // no PSRAM, UART console
  ASSERT_OK(plain.check("a", one(HwKind::Gpio, 35, "x")));
  ASSERT_OK(plain.check("a", one(HwKind::Gpio, 19, "x")));
  ASSERT_ERR("SPI flash", plain.check("a", one(HwKind::Gpio, 30, "x")));
}

static void test_strapping_warning(void) {
  HwClaims s3 = claims(HwChip::Esp32S3);
  HwCheck c = s3.check("a", one(HwKind::Gpio, 45, "relay", kHwOutput));
  ASSERT_OK(c);
  TEST_ASSERT_TRUE(c.warning);
  ASSERT_REASON("1.8 V", c);

  HwClaims e = claims(HwChip::Esp32);
  c = e.check("a", one(HwKind::Gpio, 12, "x"));
  ASSERT_OK(c);
  TEST_ASSERT_TRUE(c.warning);

  c = e.check("a", one(HwKind::Gpio, 13, "x"));
  ASSERT_OK(c);
  TEST_ASSERT_FALSE(c.warning);
  TEST_ASSERT_EQUAL_STRING("", c.reason);
}

static void test_input_only_and_adc2(void) {
  HwClaims e = claims(HwChip::Esp32);
  ASSERT_ERR("input-only", e.check("a", one(HwKind::Gpio, 34, "relay", kHwOutput)));
  ASSERT_OK(e.check("a", one(HwKind::Gpio, 34, "sense")));
  HwCheck c = e.check("a", one(HwKind::Adc, 25, "v"));
  ASSERT_OK(c);
  TEST_ASSERT_TRUE(c.warning);
  ASSERT_REASON("ADC2", c);
  c = e.check("a", one(HwKind::Adc, 36, "v"));
  ASSERT_OK(c);
  TEST_ASSERT_FALSE(c.warning);
}

static void test_conflict_between_instances(void) {
  HwClaims c = claims(HwChip::Esp32C3);
  HwDescriptor p1;
  p1.add(HwKind::Uart, 1, "port");
  p1.add(HwKind::Gpio, 4, "tx", kHwOutput);
  p1.add(HwKind::Gpio, 5, "rx");
  ASSERT_OK(c.check("meter1", p1));
  c.set("meter1", p1);
  TEST_ASSERT_EQUAL_UINT(3, c.size());

  HwDescriptor p2;
  p2.add(HwKind::Gpio, 6, "tx", kHwOutput);
  p2.add(HwKind::Gpio, 4, "rx");
  ASSERT_ERR("GPIO4 used by meter1 (tx)", c.check("meter2", p2));

  HwDescriptor p3;
  p3.add(HwKind::Uart, 1, "port");
  ASSERT_ERR("UART1 used by meter1 (port)", c.check("meter2", p3));

  // An owner's own claims never conflict with its new descriptor
  ASSERT_OK(c.check("meter1", p1));

  // set() replaces, remove() frees
  HwDescriptor p1b;
  p1b.add(HwKind::Gpio, 7, "tx");
  c.set("meter1", p1b);
  TEST_ASSERT_EQUAL_UINT(1, c.size());
  ASSERT_OK(c.check("meter2", p2));
  c.remove("meter1");
  TEST_ASSERT_EQUAL_UINT(0, c.size());

  // Owner identity is by string, not pointer
  char id[] = "meter1";
  c.set(id, p1);
  ASSERT_OK(c.check("meter1", p1));
}

static void test_duplicates_within_instance(void) {
  HwClaims c = claims(HwChip::Esp32C6);
  HwDescriptor d;
  d.add(HwKind::Gpio, 2, "tx");
  d.add(HwKind::Adc, 2, "sense");
  ASSERT_ERR("GPIO2 used twice (tx, sense)", c.check("a", d));

  HwDescriptor s;  // shared, but as two different pins of a bus: no
  s.add(HwKind::Gpio, 6, "sda", kHwShared, 0);
  s.add(HwKind::Gpio, 6, "scl", kHwShared, 0);
  ASSERT_ERR("GPIO6 used twice (sda, scl)", c.check("a", s));
}

static HwDescriptor i2cDevice(uint8_t addr, int8_t bus = 0, int16_t sda = 8, int16_t scl = 9) {
  HwDescriptor d;
  d.add(HwKind::I2cBus, bus, "bus", kHwShared);
  d.add(HwKind::Gpio, sda, "sda", kHwShared, bus);
  d.add(HwKind::Gpio, scl, "scl", kHwShared, bus);
  d.add(HwKind::I2cAddr, addr, "addr", 0, bus);
  return d;
}

static void test_shared_i2c(void) {
  HwClaims c = claims(HwChip::Esp32S3);
  c.set("rtc", i2cDevice(0x68));
  ASSERT_OK(c.check("adc", i2cDevice(0x48)));
  ASSERT_ERR("I2C0 0x68 used by rtc", c.check("rtc2", i2cDevice(0x68)));

  // the same address on the other bus: fine
  ASSERT_OK(c.check("rtc2", one(HwKind::I2cAddr, 0x68, "addr", 0, 1)));

  // a non-shared user of the SDA pin
  ASSERT_ERR("GPIO8 used by rtc (sda)", c.check("led", one(HwKind::Gpio, 8, "led", kHwOutput)));

  ASSERT_ERR("reserved", c.check("x", one(HwKind::I2cAddr, 0x03, "addr", 0, 0)));
  ASSERT_ERR("I2C2 doesn't exist", c.check("x", one(HwKind::I2cBus, 2, "bus", kHwShared)));

  // an address with no bus is on bus 0
  ASSERT_ERR("I2C0 0x68 used by rtc", c.check("x", one(HwKind::I2cAddr, 0x68, "addr")));
  ASSERT_ERR("I2C2 doesn't exist", c.check("x", one(HwKind::I2cAddr, 0x50, "addr", 0, 2)));

  // the same pins as another bus: no
  ASSERT_ERR("GPIO8 used by rtc (sda)", c.check("eeprom", i2cDevice(0x50, 1)));
  // the same bus on other pins: no (the second Wire.begin would move it)
  ASSERT_ERR("sda of bus 0 is GPIO8 (rtc)", c.check("eeprom", i2cDevice(0x50, 0, 4, 5)));
  // bus 1 on its own pins: fine
  ASSERT_OK(c.check("eeprom", i2cDevice(0x50, 1, 4, 5)));
}

static void test_spi_cs_unique(void) {
  HwClaims c = claims(HwChip::Esp32S3);
  auto dev = [](int16_t cs) {
    HwDescriptor d;
    d.add(HwKind::SpiBus, 0, "bus", kHwShared);
    d.add(HwKind::Gpio, 12, "sck", kHwShared | kHwOutput);
    d.add(HwKind::Gpio, 11, "mosi", kHwShared | kHwOutput);
    d.add(HwKind::Gpio, 13, "miso", kHwShared);
    d.add(HwKind::SpiCs, cs, "cs", kHwOutput, 0);
    return d;
  };
  c.set("adc", dev(10));
  ASSERT_OK(c.check("flash", dev(14)));
  ASSERT_ERR("GPIO10 used by adc (cs)", c.check("flash", dev(10)));
  ASSERT_ERR("GPIO10 used by adc (cs)", c.check("led", one(HwKind::Gpio, 10, "led", kHwOutput)));
  ASSERT_ERR("SPI2 doesn't exist", c.check("x", one(HwKind::SpiBus, 2, "bus", kHwShared)));
  ASSERT_ERR("SPI3 doesn't exist", c.check("x", one(HwKind::SpiCs, 15, "cs", kHwOutput, 3)));
}

static void test_c3_c6_reserved(void) {
  HwClaims c3 = claims(HwChip::Esp32C3);
  ASSERT_ERR("GPIO12 reserved: SPI flash", c3.check("a", one(HwKind::Gpio, 12, "x")));
  ASSERT_ERR("UART0 console", c3.check("a", one(HwKind::Gpio, 21, "x")));
  ASSERT_OK(c3.check("a", one(HwKind::Gpio, 18, "x")));  // USB not in use
  BoardFlags usb;
  usb.usbInUse = true;
  HwClaims c3u = claims(HwChip::Esp32C3, usb);
  ASSERT_ERR("USB", c3u.check("a", one(HwKind::Gpio, 18, "x")));
  HwCheck w = c3.check("a", one(HwKind::Gpio, 9, "x"));
  ASSERT_OK(w);
  TEST_ASSERT_TRUE(w.warning);

  HwClaims c6 = claims(HwChip::Esp32C6, usb);
  ASSERT_ERR("SPI flash", c6.check("a", one(HwKind::Gpio, 24, "x")));
  ASSERT_ERR("UART0 console", c6.check("a", one(HwKind::Gpio, 16, "x")));
  ASSERT_ERR("USB", c6.check("a", one(HwKind::Gpio, 13, "x")));
  w = c6.check("a", one(HwKind::Gpio, 4, "x"));
  ASSERT_OK(w);
  TEST_ASSERT_TRUE(w.warning);
  ASSERT_ERR("UART2 doesn't exist", c6.check("a", one(HwKind::Uart, 2, "port")));
}

static void test_channel_budgets(void) {
  HwClaims c3 = claims(HwChip::Esp32C3);
  ASSERT_ERR("no PCNT units on ESP32-C3", c3.check("a", one(HwKind::PcntUnit, kHwAnyChannel, "count")));

  c3.set("a", one(HwKind::RmtTx, kHwAnyChannel, "led"));
  ASSERT_OK(c3.check("b", one(HwKind::RmtTx, kHwAnyChannel, "led")));
  c3.set("b", one(HwKind::RmtTx, kHwAnyChannel, "led"));
  ASSERT_ERR("RMT TX channels exhausted (2 on ESP32-C3)", c3.check("c", one(HwKind::RmtTx, kHwAnyChannel, "led")));
  ASSERT_OK(c3.check("c", one(HwKind::RmtRx, kHwAnyChannel, "ir")));
  ASSERT_ERR("doesn't exist", c3.check("c", one(HwKind::RmtRx, 2, "ir")));

  // Counted within one descriptor too
  HwDescriptor two;
  two.add(HwKind::Ledc, kHwAnyChannel, "r");
  two.add(HwKind::Ledc, kHwAnyChannel, "g");
  ASSERT_OK(c3.check("rgb", two));
  HwDescriptor seven;
  for (int i = 0; i < 7; ++i) seven.add(HwKind::Ledc, kHwAnyChannel, "pwm");
  ASSERT_ERR("LEDC channels exhausted (6", c3.check("pwm", seven));

  // A fixed channel conflicts with the same fixed channel
  HwClaims s3 = claims(HwChip::Esp32S3);
  s3.set("a", one(HwKind::Ledc, 3, "fan"));
  ASSERT_ERR("ledc 3 used by a (fan)", s3.check("b", one(HwKind::Ledc, 3, "fan")));
  ASSERT_OK(s3.check("b", one(HwKind::Ledc, 4, "fan")));
}

static void test_esp32_rmt_shared_pool(void) {
  HwClaims e = claims(HwChip::Esp32);
  HwDescriptor tx;
  for (int i = 0; i < 5; ++i) tx.add(HwKind::RmtTx, kHwAnyChannel, "tx");
  e.set("a", tx);
  HwDescriptor rx;
  for (int i = 0; i < 3; ++i) rx.add(HwKind::RmtRx, kHwAnyChannel, "rx");
  ASSERT_OK(e.check("b", rx));
  rx.add(HwKind::RmtRx, kHwAnyChannel, "rx");
  ASSERT_ERR("RMT channels exhausted (8 on ESP32)", e.check("b", rx));

  // fixed channels: TX 3 and RX 3 are one channel here
  HwClaims e2 = claims(HwChip::Esp32);
  e2.set("a", one(HwKind::RmtTx, 3, "tx"));
  ASSERT_ERR("rmt-rx 3 used by a (tx)", e2.check("b", one(HwKind::RmtRx, 3, "rx")));
  ASSERT_OK(e2.check("b", one(HwKind::RmtRx, 4, "rx")));

  HwClaims s3 = claims(HwChip::Esp32S3);  // separate 4 TX + 4 RX
  s3.set("x", one(HwKind::RmtTx, 3, "tx"));
  ASSERT_OK(s3.check("y", one(HwKind::RmtRx, 3, "rx")));
  s3.remove("x");
  HwDescriptor t4;
  for (int i = 0; i < 4; ++i) t4.add(HwKind::RmtTx, kHwAnyChannel, "tx");
  s3.set("a", t4);
  HwDescriptor r4;
  for (int i = 0; i < 4; ++i) r4.add(HwKind::RmtRx, kHwAnyChannel, "rx");
  ASSERT_OK(s3.check("b", r4));
}

static void test_not_configured_skipped(void) {
  HwClaims c = claims(HwChip::Esp32C3);
  HwDescriptor d;
  d.add(HwKind::Gpio, kHwNotConfigured, "tx");
  d.add(HwKind::Gpio, kHwNotConfigured, "rx");
  ASSERT_OK(c.check("a", d));
  c.set("a", d);
  TEST_ASSERT_EQUAL_UINT(0, c.size());
  ASSERT_OK(c.check("b", d));
}

static void test_first_error_kept(void) {
  HwClaims c = claims(HwChip::Esp32);
  HwDescriptor d;
  d.add(HwKind::Gpio, 12, "x");  // caution
  d.add(HwKind::Gpio, 6, "y");   // reserved
  d.add(HwKind::Gpio, 40, "z");  // doesn't exist
  HwCheck r = c.check("a", d);
  ASSERT_ERR("GPIO6 reserved", r);
  TEST_ASSERT_FALSE(r.warning);
}

static void test_unknown_chip_permissive(void) {
  HwClaims u = claims(HwChip::Unknown);
  ASSERT_OK(u.check("a", one(HwKind::Gpio, 60, "x", kHwOutput)));
  u.set("a", one(HwKind::Gpio, 60, "x"));
  ASSERT_ERR("GPIO60 used by a", u.check("b", one(HwKind::Gpio, 60, "x")));
}

static void test_tables_consistent(void) {
  using namespace hwtab;
  // reserved pins exist on their chip
  TEST_ASSERT_EQUAL_UINT64(maskOf(kEsp32Flash), maskOf(kEsp32Flash) & kEsp32ValidMask);
  TEST_ASSERT_EQUAL_UINT64(maskOf(kS3Octal), maskOf(kS3Octal) & kS3ValidMask);
  TEST_ASSERT_EQUAL_UINT64(maskOf(kC3Usb) | maskOf(kC3Uart0), (maskOf(kC3Usb) | maskOf(kC3Uart0)) & kC3ValidMask);
  TEST_ASSERT_EQUAL_UINT64(maskOf(kC6Flash), maskOf(kC6Flash) & kC6ValidMask);
  // every chip fits its note table with every flag on
  BoardFlags all;
  all.psram = all.octalPsramOrFlash = all.usbInUse = all.uart0Console = true;
  for (HwChip ch : {HwChip::Esp32, HwChip::Esp32S3, HwChip::Esp32C3, HwChip::Esp32C6}) {
    BoardInfo b = boardInfoFor(ch, all);
    TEST_ASSERT_LESS_THAN(BoardInfo::kMaxNotes, b.noteCount);
  }
}


// SES-LIB-023: non-GPIO reservations
static void test_reserved_non_gpio(void) {
  HwClaims c = claims(HwChip::Esp32S3);
  TEST_ASSERT_TRUE(c.reserve(HwResource{HwKind::Uart, 2, -1, 0, "port"}, "modbus bridge"));
  TEST_ASSERT_FALSE(c.reserve(HwResource{HwKind::Gpio, 5, -1, 0, "x"}, "led"));  // pins: board notes
  ASSERT_ERR("UART2 reserved: modbus bridge", c.check("meter", one(HwKind::Uart, 2, "port")));
  ASSERT_OK(c.check("meter", one(HwKind::Uart, 1, "port")));
  TEST_ASSERT_EQUAL_UINT(1, c.reservedCount());

  HwClaims c3 = claims(HwChip::Esp32C3);  // 2 RMT TX: one reserved leaves one
  c3.reserve(HwResource{HwKind::RmtTx, 0, -1, 0, "tx"}, "ir blaster");
  ASSERT_ERR("rmt-tx 0 reserved: ir blaster", c3.check("a", one(HwKind::RmtTx, 0, "led")));
  ASSERT_OK(c3.check("a", one(HwKind::RmtTx, kHwAnyChannel, "led")));
  HwDescriptor two;
  two.add(HwKind::RmtTx, kHwAnyChannel, "a");
  two.add(HwKind::RmtTx, kHwAnyChannel, "b");
  ASSERT_ERR("RMT TX channels exhausted", c3.check("a", two));
}

// SES-LIB-024: a claim lives until the hardware is released
static void test_releasing(void) {
  HwClaims c = claims(HwChip::Esp32C3);
  HwDescriptor a;
  a.add(HwKind::Uart, 1, "port");
  a.add(HwKind::Gpio, 4, "tx", kHwOutput);
  c.set("meter1", a);

  HwDescriptor b;  // moves tx 4 -> 6, keeps the UART
  b.add(HwKind::Uart, 1, "port");
  b.add(HwKind::Gpio, 6, "tx", kHwOutput);
  c.replace("meter1", b, true, 0);
  TEST_ASSERT_EQUAL_UINT(1, c.releasingCount("meter1"));
  ASSERT_ERR("GPIO4 being released by meter1 (tx)", c.check("meter2", one(HwKind::Gpio, 4, "rx")));
  ASSERT_ERR("UART1 used by meter1 (port)", c.check("meter2", one(HwKind::Uart, 1, "port")));
  ASSERT_OK(c.check("meter1", a));  // never blocks its owner

  c.released("meter1");
  TEST_ASSERT_EQUAL_UINT(0, c.releasingCount("meter1"));
  ASSERT_OK(c.check("meter2", one(HwKind::Gpio, 4, "rx")));
  ASSERT_ERR("GPIO6 used by meter1", c.check("meter2", one(HwKind::Gpio, 6, "rx")));

  // taken back before it was released: active again, no duplicate
  c.replace("meter1", a, true, 0);  // 6 releasing, 4 active
  c.replace("meter1", b, true, 0);  // 4 releasing, 6 active again
  TEST_ASSERT_EQUAL_UINT(1, c.releasingCount("meter1"));
  TEST_ASSERT_EQUAL_UINT(3, c.size());  // uart1, gpio6 active + gpio4 releasing

  // losing everything (Conflict / Disabled): all releasing until released()
  c.releaseAll("meter1", true, 0);
  TEST_ASSERT_EQUAL_UINT(3, c.releasingCount("meter1"));
  ASSERT_ERR("being released by meter1", c.check("meter2", one(HwKind::Uart, 1, "port")));
  c.released("meter1");
  TEST_ASSERT_EQUAL_UINT(0, c.size());

  // a releasing channel still counts toward the budget
  HwClaims c3 = claims(HwChip::Esp32C3);
  HwDescriptor rmt;
  rmt.add(HwKind::RmtTx, kHwAnyChannel, "a");
  rmt.add(HwKind::RmtTx, kHwAnyChannel, "b");
  c3.set("x", rmt);
  c3.releaseAll("x", true, 0);
  ASSERT_ERR("RMT TX channels exhausted", c3.check("y", one(HwKind::RmtTx, kHwAnyChannel, "led")));
}

// SES-LIB-024: only an owner that set the hardware up (was Ok) holds it
// while releasing; one that never did gives it up at once.
static void test_releasing_needs_applied(void) {
  HwClaims c = claims(HwChip::Esp32C3);
  HwDescriptor a;
  a.add(HwKind::Uart, 1, "port");
  a.add(HwKind::Gpio, 4, "tx", kHwOutput);
  c.set("meter1", a);
  c.releaseAll("meter1", false, 0);  // e.g. never was Ok, now Disabled
  TEST_ASSERT_EQUAL_UINT(0, c.releasingCount("meter1"));
  TEST_ASSERT_EQUAL_UINT(0, c.size());
  ASSERT_OK(c.check("meter2", one(HwKind::Gpio, 4, "rx")));

  c.set("meter1", a);
  c.replace("meter1", one(HwKind::Gpio, 6, "tx", kHwOutput), false, 0);
  TEST_ASSERT_EQUAL_UINT(0, c.releasingCount("meter1"));
  TEST_ASSERT_EQUAL_UINT(1, c.size());  // only GPIO6
  ASSERT_OK(c.check("meter2", one(HwKind::Uart, 1, "port")));

  // claims already releasing stay so: that hardware was set up earlier
  c.replace("meter1", a, true, 0);  // GPIO6 releasing
  c.releaseAll("meter1", false, 0);  // drops the active UART1/GPIO4
  TEST_ASSERT_EQUAL_UINT(1, c.releasingCount("meter1"));
  ASSERT_ERR("GPIO6 being released by meter1", c.check("meter2", one(HwKind::Gpio, 6, "rx")));
  ASSERT_OK(c.check("meter2", one(HwKind::Gpio, 4, "rx")));
}

// SES-LIB-024: releasing too long without released() is a warning in the
// tab; the claim stays.
static void test_releasing_overdue(void) {
  HwClaims c = claims(HwChip::Esp32C3);
  c.set("meter1", one(HwKind::Gpio, 4, "tx", kHwOutput));
  TEST_ASSERT_EQUAL_UINT32(0, c.releasingAgeMs("meter1", 5000));
  c.releaseAll("meter1", true, 0xFFFFF000u);  // stamped just before millis() wraps
  TEST_ASSERT_EQUAL_UINT32(0x1000u + 3000u, c.releasingAgeMs("meter1", 3000));
  TEST_ASSERT_EQUAL_UINT(1, c.releasingCount("meter1"));  // never dropped by age

  char state[32], note[160];
  HwInstStatus s;
  s.state = HwState::Disabled;
  s.releasing = 1;
  s.releasingAgeMs = kHwReleasingWarnMs;  // not yet
  hwStatusText(s, state, sizeof(state), note, sizeof(note));
  TEST_ASSERT_EQUAL_STRING("Disabled", state);
  TEST_ASSERT_EQUAL_STRING("releasing 1", note);
  s.releasingAgeMs = 14500;
  hwStatusText(s, state, sizeof(state), note, sizeof(note));
  TEST_ASSERT_EQUAL_STRING("Disabled · warn", state);
  TEST_ASSERT_EQUAL_STRING("releasing 1 for 14 s, no released()", note);
}

// SES-LIB-025: the instance's runtime note in System -> Hardware
static void test_runtime_note_text(void) {
  char state[32], note[160];
  HwInstStatus s;
  s.state = HwState::Ok;
  hwStatusText(s, state, sizeof(state), note, sizeof(note));
  TEST_ASSERT_EQUAL_STRING("Ok", state);
  TEST_ASSERT_EQUAL_STRING("", note);

  s.note = "no answer from the meter";
  s.noteLevel = HwNoteLevel::Fault;
  hwStatusText(s, state, sizeof(state), note, sizeof(note));
  TEST_ASSERT_EQUAL_STRING("Ok · fault", state);
  TEST_ASSERT_EQUAL_STRING("no answer from the meter", note);

  s.noteLevel = HwNoteLevel::Info;  // shown, but no level on the state
  hwStatusText(s, state, sizeof(state), note, sizeof(note));
  TEST_ASSERT_EQUAL_STRING("Ok", state);

  // registry reason first, then the runtime note, then releasing
  s.reason = "GPIO9 is a strapping pin";
  s.noteLevel = HwNoteLevel::Warn;
  s.releasing = 2;
  s.releasingAgeMs = 20000;  // fault beats the releasing warning; warn stays warn
  hwStatusText(s, state, sizeof(state), note, sizeof(note));
  TEST_ASSERT_EQUAL_STRING("Ok · warn", state);
  TEST_ASSERT_EQUAL_STRING("GPIO9 is a strapping pin · no answer from the meter · releasing 2 for 20 s, no released()",
                           note);
  s.noteLevel = HwNoteLevel::Fault;
  hwStatusText(s, state, sizeof(state), note, sizeof(note));
  TEST_ASSERT_EQUAL_STRING("Ok · fault", state);

  char small[12];  // truncates, stays terminated
  hwStatusText(s, state, sizeof(state), small, sizeof(small));
  TEST_ASSERT_EQUAL_UINT(sizeof(small) - 1, strlen(small));
  TEST_ASSERT_EQUAL_STRING("Not configured", hwStateLabel(HwState::NotConfigured));

  // a cut never leaves half a UTF-8 character
  char uk[8];  // 5 Cyrillic letters, 10 bytes: 7 fit, the 4th letter is cut in half
  snprintf(uk, sizeof(uk), "%s", "\xD0\xBD\xD0\xB5\xD0\xBC\xD0\xB0\xD1\x94");
  hwUtf8Trim(uk);
  TEST_ASSERT_EQUAL_STRING("\xD0\xBD\xD0\xB5\xD0\xBC", uk);
  char whole[] = "\xD0\xBD\xD0\xB5";  // complete: kept
  hwUtf8Trim(whole);
  TEST_ASSERT_EQUAL_STRING("\xD0\xBD\xD0\xB5", whole);
  char sep[8];  // "ab · cd" cut inside the dot
  snprintf(sep, 5, "%s", "ab \xC2\xB7 cd");
  hwUtf8Trim(sep);
  TEST_ASSERT_EQUAL_STRING("ab ", sep);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_names);
  RUN_TEST(test_board_tables);
  RUN_TEST(test_flash_and_console_reserved);
  RUN_TEST(test_s3_octal_usb);
  RUN_TEST(test_strapping_warning);
  RUN_TEST(test_input_only_and_adc2);
  RUN_TEST(test_conflict_between_instances);
  RUN_TEST(test_duplicates_within_instance);
  RUN_TEST(test_shared_i2c);
  RUN_TEST(test_spi_cs_unique);
  RUN_TEST(test_c3_c6_reserved);
  RUN_TEST(test_channel_budgets);
  RUN_TEST(test_esp32_rmt_shared_pool);
  RUN_TEST(test_not_configured_skipped);
  RUN_TEST(test_first_error_kept);
  RUN_TEST(test_unknown_chip_permissive);
  RUN_TEST(test_tables_consistent);
  RUN_TEST(test_reserved_non_gpio);
  RUN_TEST(test_releasing);
  RUN_TEST(test_releasing_needs_applied);
  RUN_TEST(test_releasing_overdue);
  RUN_TEST(test_runtime_note_text);
  return UNITY_END();
}
