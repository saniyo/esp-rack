// Host tests of the file manager's path rules (modules/filesystem FsPath.h)
// and an upload's promotion (FsPromote.h) on a fake volume, cut short at
// every step as a restart or a power loss would.

#include <unity.h>

#include <map>
#include <string>

#include "../../../modules/filesystem/include/FsPath.h"
#include "../../../modules/filesystem/include/FsPromote.h"

using Str = std::string;

void setUp(void) {}
void tearDown(void) {}

static Str norm(const char* in) {
  Str out = "unchanged";
  return fsNormalise(Str(in), out) ? out : Str("<refused>");
}

static void test_normalise(void) {
  TEST_ASSERT_EQUAL_STRING("/flash/config", norm("/flash/./config").c_str());
  TEST_ASSERT_EQUAL_STRING("/flash/config", norm("/flash//config/").c_str());
  TEST_ASSERT_EQUAL_STRING("/flash/config", norm("//flash/.//./config//.").c_str());
  TEST_ASSERT_EQUAL_STRING("/", norm("/").c_str());
  TEST_ASSERT_EQUAL_STRING("/", norm("/./").c_str());
  TEST_ASSERT_EQUAL_STRING("/flash", norm("/flash/").c_str());
  TEST_ASSERT_EQUAL_STRING("/flash/..x/.y", norm("/flash/..x/.y").c_str());  // names, not segments
  TEST_ASSERT_EQUAL_STRING("<refused>", norm("/flash/../flash/config").c_str());
  TEST_ASSERT_EQUAL_STRING("<refused>", norm("/flash/x/..").c_str());
  TEST_ASSERT_EQUAL_STRING("<refused>", norm("flash/x").c_str());
  TEST_ASSERT_EQUAL_STRING("<refused>", norm("").c_str());
}

static void test_within(void) {
  const Str cfg = "/flash/config";
  TEST_ASSERT_TRUE(fsWithin(Str("/flash/config"), cfg));
  TEST_ASSERT_TRUE(fsWithin(Str("/flash/config/a.json"), cfg));
  TEST_ASSERT_FALSE(fsWithin(Str("/flash/configs"), cfg));
  TEST_ASSERT_FALSE(fsWithin(Str("/flash"), cfg));
  // A tree that holds it: the parent, the volume, the root.
  TEST_ASSERT_TRUE(fsWithin(cfg, Str("/flash")));
  TEST_ASSERT_TRUE(fsWithin(cfg, Str("/")));
  TEST_ASSERT_FALSE(fsWithin(cfg, Str("/fla")));
  TEST_ASSERT_FALSE(fsWithin(cfg, Str("/sd")));
}

// A volume: path -> contents. replace: a rename onto a file replaces it (LittleFS), else refused (FAT).
struct FakeVol {
  using Str = std::string;
  std::map<Str, Str> files;
  bool replace = true;
  int ops = 0;
  int cutAfter = -1;  // a restart after this many operations: nothing more happens

  bool live() { return cutAfter < 0 || ops < cutAfter; }
  bool replaces() const { return replace; }
  bool exists(const Str& p) { return files.count(p) != 0; }
  bool remove(const Str& p) {
    if (!live()) return false;
    ++ops;
    return files.erase(p) != 0;
  }
  FsResult rename(const Str& from, const Str& to) {
    if (!live()) return FsResult::IO_ERROR;
    ++ops;
    auto it = files.find(from);
    if (it == files.end()) return FsResult::NOT_FOUND;
    if (!replace && files.count(to)) return FsResult::EXISTS;
    files[to] = it->second;
    files.erase(from);
    return FsResult::OK;
  }
};

using P = FsPromote<FakeVol>;

// Runs the promotion to its end (or until a call fails); the calls made.
static int promote(FakeVol& v, const Str& target, bool hashOk, P::Next& last, FsResult& err) {
  uint8_t at = 0;
  int calls = 0;
  err = FsResult::OK;
  do {
    last = P::step(v, at, target, hashOk, err);
    ++calls;
  } while (last == P::Next::More && calls < 20);
  return calls;
}

// At boot: .partial removed, .partial.old settled (FileSystemManager::purgeUploadDebris).
static void boot(FakeVol& v) {
  v.cutAfter = -1;
  std::map<Str, Str> copy = v.files;
  for (auto& f : copy) {
    const Str& n = f.first;
    if (n.size() > 8 && n.compare(n.size() - 8, 8, ".partial") == 0) v.files.erase(n);
  }
  copy = v.files;
  for (auto& f : copy) {
    const Str& n = f.first;
    if (n.size() > 12 && n.compare(n.size() - 12, 12, ".partial.old") == 0) (void)P::restoreOld(v, n);
  }
}

static void test_promote_replaces(void) {
  FakeVol v;
  v.files = {{"/f", "old"}, {"/f.partial", "new"}};
  P::Next last;
  FsResult err;
  TEST_ASSERT_EQUAL_INT(1, promote(v, "/f", true, last, err));  // one atomic rename
  TEST_ASSERT_TRUE(last == P::Next::Done);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)v.files.size());
  TEST_ASSERT_EQUAL_STRING("new", v.files["/f"].c_str());
  // No old one: the same.
  v.files = {{"/g.partial", "new"}};
  TEST_ASSERT_EQUAL_INT(1, promote(v, "/g", true, last, err));
  TEST_ASSERT_EQUAL_STRING("new", v.files["/g"].c_str());
}

static void test_promote_aside(void) {
  FakeVol v;
  v.replace = false;
  v.files = {{"/f", "old"}, {"/f.partial", "new"}, {"/f.partial.old", "stale"}};
  P::Next last;
  FsResult err;
  TEST_ASSERT_EQUAL_INT(4, promote(v, "/f", true, last, err));  // stale out, aside, in, old out
  TEST_ASSERT_TRUE(last == P::Next::Done);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)v.files.size());
  TEST_ASSERT_EQUAL_STRING("new", v.files["/f"].c_str());
  // No old one: one rename.
  v.files = {{"/g.partial", "new"}};
  TEST_ASSERT_EQUAL_INT(1, promote(v, "/g", true, last, err));
  TEST_ASSERT_EQUAL_STRING("new", v.files["/g"].c_str());
}

// Cut short after every operation, on both kinds of volume: after the boot
// the target holds the old file or the new one, never nothing.
static void test_promote_cut_anywhere(void) {
  for (int replace = 0; replace < 2; ++replace) {
    for (int cut = 0; cut <= 4; ++cut) {
      FakeVol v;
      v.replace = replace != 0;
      v.files = {{"/d/f", "old"}, {"/d/f.partial", "new"}};
      v.cutAfter = cut;
      P::Next last;
      FsResult err;
      (void)promote(v, "/d/f", true, last, err);
      boot(v);
      TEST_ASSERT_TRUE(v.exists("/d/f"));
      const Str& got = v.files["/d/f"];
      TEST_ASSERT_TRUE(got == "old" || got == "new");
      TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)v.files.size());  // no debris left
    }
  }
}

static void test_promote_hash_mismatch(void) {
  for (int replace = 0; replace < 2; ++replace) {
    FakeVol v;
    v.replace = replace != 0;
    v.files = {{"/f", "old"}, {"/f.partial", "bad"}, {"/f.corrupt", "older bad"}};
    P::Next last;
    FsResult err;
    (void)promote(v, "/f", false, last, err);
    TEST_ASSERT_TRUE(last == P::Next::Failed);
    TEST_ASSERT_TRUE(err == FsResult::OK);  // the caller reports the mismatch
    TEST_ASSERT_EQUAL_STRING("old", v.files["/f"].c_str());
    TEST_ASSERT_EQUAL_STRING("bad", v.files["/f.corrupt"].c_str());
    TEST_ASSERT_FALSE(v.exists("/f.partial"));
  }
}

// The new one can't go in (FAT): the old one goes back, the error is said.
static void test_promote_fails_back(void) {
  struct Vol : FakeVol {
    FsResult rename(const Str& from, const Str& to) {
      if (from == "/f.partial") return FsResult::IO_ERROR;
      return FakeVol::rename(from, to);
    }
  } v;
  v.replace = false;
  v.files = {{"/f", "old"}, {"/f.partial", "new"}};
  uint8_t at = 0;
  FsResult err = FsResult::OK;
  using PV = FsPromote<Vol>;
  TEST_ASSERT_TRUE(PV::step(v, at, "/f", true, err) == PV::Next::More);
  TEST_ASSERT_TRUE(PV::step(v, at, "/f", true, err) == PV::Next::Failed);
  TEST_ASSERT_TRUE(err == FsResult::IO_ERROR);
  TEST_ASSERT_EQUAL_STRING("old", v.files["/f"].c_str());
  TEST_ASSERT_FALSE(v.exists("/f.partial.old"));
}

static void test_restore_old_names(void) {
  FakeVol v;
  v.files = {{"/x.partial.old", "a"}};
  TEST_ASSERT_TRUE(P::restoreOld(v, "/x.partial.old"));
  TEST_ASSERT_EQUAL_STRING("a", v.files["/x"].c_str());
  TEST_ASSERT_FALSE(P::restoreOld(v, ".partial.old"));  // no name left
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_normalise);
  RUN_TEST(test_within);
  RUN_TEST(test_promote_replaces);
  RUN_TEST(test_promote_aside);
  RUN_TEST(test_promote_cut_anywhere);
  RUN_TEST(test_promote_hash_mismatch);
  RUN_TEST(test_promote_fails_back);
  RUN_TEST(test_restore_old_names);
  return UNITY_END();
}
