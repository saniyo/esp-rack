// PresenceLogQueue: the presence log lines the flash write gate put off,
// bounded by lines and bytes (the oldest go, counted), written in batches
// of up to a slot's worth of bytes.
#include <unity.h>

#include <string>

#include "../../../modules/presence/include/PresenceLogQueue.h"

using Q = PresenceLogQueue<std::string>;

void setUp(void) {}
void tearDown(void) {}

static std::string line(char c, size_t n) { return std::string(n - 2, c) + "\r\n"; }

static void test_fifo_batch(void) {
  Q q(8, 1000);
  TEST_ASSERT_TRUE(q.empty());
  TEST_ASSERT_EQUAL_UINT(0, q.push(line('a', 10)));
  TEST_ASSERT_EQUAL_UINT(0, q.push(line('b', 10)));
  TEST_ASSERT_EQUAL_UINT(0, q.push(line('c', 10)));
  TEST_ASSERT_EQUAL_UINT(3, q.lines());
  TEST_ASSERT_EQUAL_UINT(30, q.bytes());
  std::string out;
  TEST_ASSERT_EQUAL_UINT(3, q.take(out, 4096));
  TEST_ASSERT_TRUE(out == line('a', 10) + line('b', 10) + line('c', 10));
  TEST_ASSERT_TRUE(q.empty());
  TEST_ASSERT_EQUAL_UINT(0, q.bytes());
  TEST_ASSERT_EQUAL_UINT(0, q.dropped());
}

// A batch is a slot's worth: up to maxBytes, the rest stays in order.
static void test_batch_bounded(void) {
  Q q(64, 10000);
  for (char c = 'a'; c < 'a' + 10; ++c) q.push(line(c, 100));
  std::string out;
  TEST_ASSERT_EQUAL_UINT(4, q.take(out, 450));
  TEST_ASSERT_EQUAL_UINT(400, out.size());
  TEST_ASSERT_TRUE(out.substr(0, 100) == line('a', 100));
  TEST_ASSERT_EQUAL_UINT(6, q.lines());
  TEST_ASSERT_EQUAL_UINT(600, q.bytes());
  std::string next;
  TEST_ASSERT_EQUAL_UINT(4, q.take(next, 400));  // exactly the cap fits
  TEST_ASSERT_TRUE(next.substr(0, 100) == line('e', 100));
}

// A line longer than the batch still goes, alone.
static void test_long_line_alone(void) {
  Q q(64, 10000);
  q.push(line('x', 500));
  q.push(line('y', 10));
  std::string out;
  TEST_ASSERT_EQUAL_UINT(1, q.take(out, 100));
  TEST_ASSERT_EQUAL_UINT(500, out.size());
  TEST_ASSERT_EQUAL_UINT(1, q.lines());
}

// Full by lines: the oldest go, counted.
static void test_drop_oldest_lines(void) {
  Q q(3, 10000);
  for (char c = 'a'; c < 'a' + 5; ++c) q.push(line(c, 10));
  TEST_ASSERT_EQUAL_UINT(3, q.lines());
  TEST_ASSERT_EQUAL_UINT(30, q.bytes());
  TEST_ASSERT_EQUAL_UINT(2, q.dropped());
  std::string out;
  q.take(out, 4096);
  TEST_ASSERT_TRUE(out == line('c', 10) + line('d', 10) + line('e', 10));
}

// Full by bytes: the oldest go; the newest stays even when it alone is over.
static void test_drop_oldest_bytes(void) {
  Q q(64, 250);
  q.push(line('a', 100));
  q.push(line('b', 100));
  TEST_ASSERT_EQUAL_UINT(1, q.push(line('c', 100)));  // 300 > 250: 'a' goes
  TEST_ASSERT_EQUAL_UINT(200, q.bytes());
  TEST_ASSERT_EQUAL_UINT(2, q.push(line('d', 300)));  // over alone: it stays, the rest go
  TEST_ASSERT_EQUAL_UINT(1, q.lines());
  TEST_ASSERT_EQUAL_UINT(300, q.bytes());
  TEST_ASSERT_EQUAL_UINT(3, q.dropped());
  q.lost(5);
  TEST_ASSERT_EQUAL_UINT(8, q.dropped());
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_fifo_batch);
  RUN_TEST(test_batch_bounded);
  RUN_TEST(test_long_line_alone);
  RUN_TEST(test_drop_oldest_lines);
  RUN_TEST(test_drop_oldest_bytes);
  return UNITY_END();
}
