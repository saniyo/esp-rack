// [host]: bot command parsing and who may run what.
#include <unity.h>

#include "../../../modules/telegram/include/TelegramCommand.h"

void setUp(void) {}
void tearDown(void) {}

static void test_parse(void) {
  tgcmd::Parsed p = tgcmd::parse("/On@SesBot 30 min ");
  TEST_ASSERT_TRUE(p.ok);
  TEST_ASSERT_EQUAL_STRING("on", p.command.c_str());
  TEST_ASSERT_EQUAL_STRING("30 min", p.args.c_str());
  p = tgcmd::parse("/status");
  TEST_ASSERT_TRUE(p.ok);
  TEST_ASSERT_EQUAL_STRING("status", p.command.c_str());
  TEST_ASSERT_EQUAL_STRING("", p.args.c_str());
  TEST_ASSERT_FALSE(tgcmd::parse("hello").ok);
  TEST_ASSERT_FALSE(tgcmd::parse("/").ok);
  TEST_ASSERT_FALSE(tgcmd::parse("/@bot").ok);
  TEST_ASSERT_FALSE(tgcmd::parse(nullptr).ok);
}

static void test_lists(void) {
  const char* allowed = "123, -100456 ,-100789:12";
  TEST_ASSERT_TRUE(tgcmd::chatAllowed(allowed, "123", 0, ""));
  TEST_ASSERT_TRUE(tgcmd::chatAllowed(allowed, "-100456", 7, ""));   // a group: any topic
  TEST_ASSERT_TRUE(tgcmd::chatAllowed(allowed, "-100789", 12, ""));  // only topic 12
  TEST_ASSERT_FALSE(tgcmd::chatAllowed(allowed, "-100789", 5, ""));
  TEST_ASSERT_FALSE(tgcmd::chatAllowed(allowed, "12", 0, ""));       // no prefix matches
  TEST_ASSERT_FALSE(tgcmd::chatAllowed(allowed, "999", 0, "999"));   // a list replaces the default
  TEST_ASSERT_TRUE(tgcmd::chatAllowed("", "999", 0, "999"));         // empty: the default chat only
  TEST_ASSERT_FALSE(tgcmd::chatAllowed("", "998", 0, "999"));
  TEST_ASSERT_TRUE(tgcmd::inList("111, 222", "222"));
  TEST_ASSERT_FALSE(tgcmd::inList("111, 2222", "222"));
  TEST_ASSERT_FALSE(tgcmd::inList("", "1"));
}

static void test_decide(void) {
  using tgcmd::Verdict;
  TEST_ASSERT_EQUAL_INT((int)Verdict::Ignore, (int)tgcmd::decide(false, true, false, true));
  TEST_ASSERT_EQUAL_INT((int)Verdict::Ignore, (int)tgcmd::decide(true, false, false, true));  // unknown
  TEST_ASSERT_EQUAL_INT((int)Verdict::AdminOnly, (int)tgcmd::decide(true, true, true, false));
  TEST_ASSERT_EQUAL_INT((int)Verdict::Run, (int)tgcmd::decide(true, true, true, true));
  TEST_ASSERT_EQUAL_INT((int)Verdict::Run, (int)tgcmd::decide(true, true, false, false));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_parse);
  RUN_TEST(test_lists);
  RUN_TEST(test_decide);
  return UNITY_END();
}
