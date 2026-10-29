// Host tests of the OTA write steps (system/FlashSteps, the steps
// UploadPumpCore hands a sink) against a model of IDF 5.4's sequential
// esp_ota_write: each step erases one 4 KB sector at most, and nothing is
// erased up front.

#include <unity.h>

#include <net/UploadPumpCore.h>
#include <system/FlashSteps.h>

#include <vector>

using namespace ESPRack;
using A = UploadPumpCore::Action;
using Span = UploadPumpCore::Span;

// esp_ota_begin(OTA_WITH_SEQUENTIAL_WRITES) + esp_ota_write
// (components/app_update/esp_ota_ops.c, need_erase): the erase_range
// calls it makes.
struct FakeOta {
  static constexpr uint32_t kSec = 4096;
  uint32_t wrote = 0;
  std::vector<uint32_t> eraseSizes;  // one per esp_partition_erase_range call
  void begin() {
    wrote = 0;
    eraseSizes.clear();  // sequential: nothing erased here
  }
  void write(uint32_t size) {
    const uint32_t first = wrote / kSec;
    const uint32_t last = (wrote + size - 1) / kSec;
    if (wrote % kSec == 0) {
      eraseSizes.push_back((last - first + 1) * kSec);
    } else if (first != last) {
      eraseSizes.push_back((last - first) * kSec);
    }
    wrote += size;
  }
  uint32_t sectors() const {
    uint32_t n = 0;
    for (uint32_t s : eraseSizes) n += s / kSec;
    return n;
  }
};

static FakeOta ota;
static UploadPumpCore c;
static std::vector<uint8_t> ring;
static uint32_t seed = 1;
static uint32_t rnd(uint32_t n) {
  seed = seed * 1103515245u + 12345u;
  return (seed >> 8) % n;
}

void setUp(void) {
  ota.begin();
  c = UploadPumpCore{};
  seed = 1;
}
void tearDown(void) {}

static void test_model_matches_flash_steps(void) {
  for (int i = 0; i < 20000; ++i) {
    const uint32_t offset = rnd(1u << 20);
    const uint32_t len = 1 + rnd(9000);
    FakeOta f;
    f.wrote = offset;
    f.write(len);
    TEST_ASSERT_EQUAL_UINT32(FlashSteps::erases(offset, len), f.sectors());
  }
}

static void test_why_steps_of_4k(void) {
  // One 64 KB write on a 64 KB boundary: one erase_range of 64 KB, which
  // the flash driver runs as a single block erase (cache off all along).
  ota.write(65536);
  TEST_ASSERT_EQUAL_UINT32(1, ota.eraseSizes.size());
  TEST_ASSERT_EQUAL_UINT32(65536, ota.eraseSizes[0]);
  ota.begin();
  ota.write(8192);  // two sectors in one call
  TEST_ASSERT_EQUAL_UINT32(8192, ota.eraseSizes[0]);
}

static void test_download_steps_erase_one_sector_each(void) {
  // auto-update: read 4 KB, write it in a slot.
  const uint32_t image = 1000003;
  uint32_t done = 0;
  while (done < image) {
    const uint32_t n = image - done < 4096 ? image - done : 4096;
    const size_t before = ota.eraseSizes.size();
    ota.write(n);
    TEST_ASSERT_EQUAL_UINT32(before + 1, ota.eraseSizes.size());
    TEST_ASSERT_EQUAL_UINT32(4096, ota.eraseSizes.back());
    done += n;
  }
  TEST_ASSERT_EQUAL_UINT32((image + 4095) / 4096, ota.sectors());
}

static void test_pump_steps_erase_one_sector_at_most(void) {
  // Chunks of up to 1460 B: the first few straight through (gate free),
  // the rest through the ring in steps, each step's one or two writes
  // inside one slot.
  const uint32_t holdBelow = 5760 + 2 * 1436;
  ring.assign(FlashSteps::kSector + holdBelow, 0);
  TEST_ASSERT_TRUE(c.begin(holdBelow, 0));
  c.setRing(ring.data(), static_cast<uint32_t>(ring.size()));
  TEST_ASSERT_TRUE(c.claim());
  c.opened(true);
  for (int i = 0; i < 3; ++i) {
    const uint32_t n = 1 + rnd(1460);
    ota.write(n);
    c.wrote(n, true, false);
  }
  c.done();
  const uint32_t image = 900000;
  uint32_t pushed = c.written();
  uint32_t steps = 0;
  while (pushed < image || c.used()) {
    if (pushed == image) c.final();  // all in: the last step may be short
    if (pushed < image && rnd(3)) {
      uint32_t n = 1 + rnd(1460);
      if (n > image - pushed) n = image - pushed;
      if (n <= c.room()) {
        Span s[2];
        TEST_ASSERT_TRUE(c.reserve(n, s));
        c.commit(n);
        pushed += n;
      }
      continue;
    }
    Span s[2];
    uint32_t len = 0;
    if (c.next(s, len) != A::Write) continue;
    const size_t before = ota.eraseSizes.size();
    const uint32_t at = ota.wrote;
    ota.write(s[0].len);
    if (s[1].len) ota.write(s[1].len);
    TEST_ASSERT_LESS_OR_EQUAL(before + 1, ota.eraseSizes.size());
    if (ota.eraseSizes.size() > before) TEST_ASSERT_EQUAL_UINT32(4096, ota.eraseSizes.back());
    if (steps++) TEST_ASSERT_EQUAL_UINT32(0, at % 4096);  // after the first: whole sectors
    c.wrote(len, true, true);
    c.done();
  }
  TEST_ASSERT_EQUAL_UINT32(image, ota.wrote);
  TEST_ASSERT_EQUAL_UINT32((image + 4095) / 4096, ota.sectors());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_model_matches_flash_steps);
  RUN_TEST(test_why_steps_of_4k);
  RUN_TEST(test_download_steps_erase_one_sector_each);
  RUN_TEST(test_pump_steps_erase_one_sector_at_most);
  return UNITY_END();
}
