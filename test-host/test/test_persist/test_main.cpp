// [host]: CheckpointPolicy, PowerGuard, NvsRecord (over a fake
// NVS) and RtcScratch's A/B slots.
#include <unity.h>

#include <string.h>

#include <map>
#include <string>
#include <vector>

#include <persist/CheckpointPolicy.h>
#include <persist/NvsRecord.h>
#include <persist/RtcScratch.h>
#include <power/PowerGuard.h>

using namespace ESPRack;

// ---- fakes for the target-only parts ----------------------------------------
static std::map<std::string, std::vector<uint8_t>> g_nvs;
static bool g_nvsUp = true;
static bool g_trusted = true;

namespace ESPRack {
bool rtcScratchTrusted() { return g_trusted; }
namespace nvsrec {
Io get(const char* ns, const char* key, uint8_t* buf, size_t cap, size_t& size) {
  if (!g_nvsUp) return Io::NoStore;
  auto it = g_nvs.find(std::string(ns) + "/" + key);
  if (it == g_nvs.end()) return Io::Missing;
  size = it->second.size();
  if (size > cap) return Io::TooBig;
  memcpy(buf, it->second.data(), size);
  return Io::Ok;
}
Io set(const char* ns, const char* key, const uint8_t* buf, size_t size) {
  if (!g_nvsUp) return Io::NoStore;
  g_nvs[std::string(ns) + "/" + key].assign(buf, buf + size);
  return Io::Ok;
}
}  // namespace nvsrec
}  // namespace ESPRack

void setUp(void) {
  g_nvs.clear();
  g_nvsUp = true;
  g_trusted = true;
  PowerGuard::resetForTest();
}
void tearDown(void) {}

#define ASSERT_REC(exp, got) TEST_ASSERT_EQUAL_INT((int)RecStatus::exp, (int)(got))
#define ASSERT_STORE(exp, got) TEST_ASSERT_EQUAL_INT((int)StoreStatus::exp, (int)(got))

static void test_crc32(void) {
  TEST_ASSERT_EQUAL_UINT32(0xCBF43926u, crc32Ieee("123456789", 9));
  TEST_ASSERT_EQUAL_UINT32(crc32Ieee("123456789", 9), crc32Ieee("6789", 4, crc32Ieee("12345", 5)));
}

static void test_checkpoint_policy(void) {
  const CheckpointPolicy p;  // 60 s, 15 min on, 6 h off, delta 20
  struct Row {
    uint32_t elapsed;
    bool on;
    int64_t delta;
    bool due;
  };
  const Row rows[] = {
      {59999, true, 1000, false},  // never before minInterval
      {60000, true, 20, true},     // moved enough
      {60000, false, -20, true},   // either sign
      {60000, true, 19, false},
      {899999, true, 0, false},
      {900000, true, 0, true},     // at the latest while on
      {900000, false, 0, false},
      {21600000, false, 0, true},  // at the latest while off
  };
  for (const Row& r : rows) TEST_ASSERT_EQUAL_INT(r.due ? 1 : 0, p.due(1000 + r.elapsed, 1000, r.on, r.delta) ? 1 : 0);
  TEST_ASSERT_TRUE(p.due(59999u, 0xFFFF0000u, true, 20));  // millis() wrapped
}

static void test_power_guard(void) {
  TEST_ASSERT_TRUE(PowerGuard::writesAllowed());  // Unknown: nobody senses the supply
  int calls = 0;
  SupplyState seen = SupplyState::Unknown;
  TEST_ASSERT_EQUAL_INT(0, PowerGuard::subscribe([&](SupplyState s) {
    ++calls;
    seen = s;
  }));
  PowerGuard::setSupply(SupplyState::UsbOnly, 300);
  TEST_ASSERT_TRUE(PowerGuard::writesAllowed());
  TEST_ASSERT_EQUAL_INT32(300, PowerGuard::millivolts());
  PowerGuard::setSupply(SupplyState::Fault);
  TEST_ASSERT_TRUE(PowerGuard::writesAllowed());
  PowerGuard::setSupply(SupplyState::PowerFail);
  TEST_ASSERT_FALSE(PowerGuard::writesAllowed());
  PowerGuard::setSupply(SupplyState::PowerFail);  // no change, no call
  TEST_ASSERT_EQUAL_INT(3, calls);
  PowerGuard::latchWritesOffUntilRestart();
  PowerGuard::setSupply(SupplyState::AcOk);
  TEST_ASSERT_TRUE(PowerGuard::latched());
  TEST_ASSERT_FALSE(PowerGuard::writesAllowed());  // until restart
  TEST_ASSERT_EQUAL_INT((int)SupplyState::AcOk, (int)PowerGuard::supply());  // the real supply
  TEST_ASSERT_EQUAL_INT((int)SupplyState::AcOk, (int)seen);
}

struct Rec {
  uint32_t a;
  uint32_t b;
};
struct RecV0 {
  uint32_t a;
};
struct Big {
  uint32_t w[8];
};

static bool migrateV0(uint16_t from, const uint8_t* p, size_t len, Rec& out) {
  if (from != 0 || len != sizeof(RecV0)) return false;
  RecV0 v;
  memcpy(&v, p, sizeof(v));
  out = Rec{v.a, 7};
  return true;
}
static bool migrateBig(uint16_t from, const uint8_t* p, size_t len, Rec& out) {
  if (from != 0 || len != sizeof(Big)) return false;
  memcpy(&out.a, p + 28, sizeof(out.a));
  out.b = 0;
  return true;
}

static void test_nvs_record(void) {
  NvsRecord<Rec> r("t", "rec", 1);
  Rec x{};
  ASSERT_REC(Missing, r.load(x));
  ASSERT_STORE(Ok, r.store(Rec{1, 2}));
  ASSERT_STORE(Unchanged, r.store(Rec{1, 2}));
  TEST_ASSERT_EQUAL_UINT32(1, r.generation());

  NvsRecord<Rec> r2("t", "rec", 1);  // after a reboot
  ASSERT_REC(Ok, r2.load(x));
  TEST_ASSERT_EQUAL_UINT32(2, x.b);
  TEST_ASSERT_EQUAL_UINT32(1, r2.generation());
  ASSERT_STORE(Unchanged, r2.store(x));  // loaded == stored
  ASSERT_STORE(Ok, r2.store(Rec{3, 4}));
  TEST_ASSERT_EQUAL_UINT32(2, r2.generation());
  TEST_ASSERT_EQUAL_UINT32(1, r2.writes());

  PowerGuard::setSupply(SupplyState::PowerFail);
  ASSERT_STORE(Skipped, r2.store(Rec{5, 6}));
  TEST_ASSERT_EQUAL_UINT32(1, r2.skips());
  PowerGuard::setSupply(SupplyState::AcOk);
  ASSERT_STORE(Ok, r2.store(Rec{5, 6}));

  std::vector<uint8_t>& blob = g_nvs["t/rec"];
  blob.back() = static_cast<uint8_t>(blob.back() ^ 0x01u);  // a flipped payload bit
  ASSERT_REC(Corrupt, NvsRecord<Rec>("t", "rec", 1).load(x));
  blob.pop_back();  // a short blob
  ASSERT_REC(Corrupt, NvsRecord<Rec>("t", "rec", 1).load(x));

  g_nvsUp = false;
  ASSERT_REC(NoStore, NvsRecord<Rec>("t", "rec", 1).load(x));
  ASSERT_STORE(NoStore, NvsRecord<Rec>("t", "rec", 1).store(x));
}

static void test_nvs_migrate(void) {
  NvsRecord<RecV0> old("t", "m", 0);
  ASSERT_STORE(Ok, old.store(RecV0{42}));
  NvsRecord<Rec> r("t", "m", 1);
  Rec x{};
  ASSERT_REC(Corrupt, r.load(x));  // an old schema nobody migrates
  ASSERT_REC(Migrated, r.load(x, migrateV0));
  TEST_ASSERT_EQUAL_UINT32(42, x.a);
  TEST_ASSERT_EQUAL_UINT32(7, x.b);
  ASSERT_STORE(Ok, r.store(x));  // rewritten in schema 1
  TEST_ASSERT_EQUAL_UINT32(2, r.generation());
  ASSERT_REC(Ok, NvsRecord<Rec>("t", "m", 1).load(x));

  NvsRecord<Big> big("t", "b", 0);  // an older schema, larger than the new T
  ASSERT_STORE(Ok, big.store(Big{{1, 2, 3, 4, 5, 6, 7, 8}}));
  ASSERT_REC(Migrated, NvsRecord<Rec>("t", "b", 1).load(x, migrateBig));
  TEST_ASSERT_EQUAL_UINT32(8, x.a);
}

struct Ck {
  uint32_t n;
  uint8_t pad[12];
};
static RtcScratchSlots<Ck> g_slots;

static void test_rtc_scratch(void) {
  memset(&g_slots, 0xA5, sizeof(g_slots));  // garbage, as after power-on
  {
    RtcScratch<Ck> s(g_slots);  // trusted reset, but nothing valid in there
    TEST_ASSERT_FALSE(s.valid());
  }
  g_trusted = false;  // power-on: whatever is there is wiped
  {
    RtcScratch<Ck> s(g_slots);
    TEST_ASSERT_FALSE(s.valid());
    s.commit(Ck{1, {}});
    TEST_ASSERT_TRUE(s.valid());
    TEST_ASSERT_EQUAL_UINT32(1, s.seq());
    s.commit(Ck{2, {}});  // into the other slot
    TEST_ASSERT_EQUAL_UINT32(2, s.seq());
    TEST_ASSERT_EQUAL_UINT32(1, g_slots.s[0].payload.n);
    TEST_ASSERT_EQUAL_UINT32(2, g_slots.s[1].payload.n);
  }
  g_trusted = true;  // soft reset: the newest image survives
  {
    RtcScratch<Ck> s(g_slots);
    Ck c{};
    TEST_ASSERT_TRUE(s.get(c));
    TEST_ASSERT_EQUAL_UINT32(2, c.n);
    g_slots.s[0].magic = 0;  // a reset halfway through rewriting the older slot
    TEST_ASSERT_TRUE(s.get(c));
    TEST_ASSERT_EQUAL_UINT32(2, c.n);
    s.commit(Ck{3, {}});  // goes to slot 0
    TEST_ASSERT_EQUAL_UINT32(3, g_slots.s[0].payload.n);
    g_slots.s[0].payload.n ^= 0x40u;  // the newest torn: the older one wins
    TEST_ASSERT_TRUE(s.get(c));
    TEST_ASSERT_EQUAL_UINT32(2, c.n);
    s.invalidate();
    TEST_ASSERT_FALSE(s.valid());
  }
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_crc32);
  RUN_TEST(test_checkpoint_policy);
  RUN_TEST(test_power_guard);
  RUN_TEST(test_nvs_record);
  RUN_TEST(test_nvs_migrate);
  RUN_TEST(test_rtc_scratch);
  return UNITY_END();
}
