// Host tests of the app <-> loader records (system/OtaHandoff): encode /
// decode, the CRC, truncated and foreign blobs, another version, a version
// 1 handoff read as version 2, fields out of range, and the signature
// trailer of a signed image (parse, encode, streaming hold-back).

#include <unity.h>

#include <persist/Crc32.h>
#include <system/OtaHandoff.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace ESPRack;
using namespace ESPRack::ota;

namespace {

Handoff pullHandoff() {
  Handoff h;
  clear(h, Mode::Pull);
  setText(h.url, "http://10.0.0.5:5000/update?dev=gw&ver=1.2");
  setText(h.token, "tok-123");
  h.size = 1250000;
  for (size_t i = 0; i < kSha256Len; ++i) h.sha256[i] = uint8_t(i + 1);
  h.sigLen = 71;
  for (size_t i = 0; i < h.sigLen; ++i) h.sig[i] = uint8_t(0xA0 + i);
  setText(h.targetVersion, "1.3");
  setText(h.wifiSsid, "site-ap");
  setText(h.wifiPass, "secret");
  h.attemptsAllowed = 5;
  h.adminKdf = uint8_t(AdminKdf::Pbkdf2Sha256);
  h.adminSaltLen = 16;
  h.adminIter = 2000;
  for (size_t i = 0; i < 16; ++i) h.adminSalt[i] = uint8_t(0x10 + i);
  for (size_t i = 0; i < kAdminKeyLen; ++i) h.adminKey[i] = uint8_t(0xC0 ^ i);
  setText(h.adminUser, "admin");
  setText(h.apPass, "k7mq-4hzt-p9xw");
  setText(h.deviceId, "gw-404cca4798f4-1a2b3c4d");
  return h;
}

// What an app of handoff version 1 stored: the same first fields, then the
// hex SHA-256 of the page's password (SHA-256("admin") here).
std::vector<uint8_t> v1Blob(const char* adminHash) {
  HandoffV1 v;
  memset(&v, 0, sizeof(v));
  v.magic = kHandoffMagic;
  v.version = kHandoffVersionV1;
  v.mode = uint8_t(Mode::Push);
  v.attemptsAllowed = 3;
  setText(v.url, "http://h/x.bin");
  setText(v.token, "tok");
  v.size = 4096;
  v.sha256[0] = 0xAB;
  v.sigLen = 70;
  v.sig[69] = 0x5A;
  setText(v.targetVersion, "1.3");
  setText(v.wifiSsid, "site-ap");
  setText(v.wifiPass, "secret");
  v.useShared = 0;
  setText(v.adminHash, adminHash);
  v.crc = crc32Ieee(&v, offsetof(HandoffV1, crc));
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  return std::vector<uint8_t>(p, p + sizeof(v));
}

const char kSha256Admin[] = "8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918";

}  // namespace

void setUp() {}
void tearDown() {}

void test_handoff_round_trip() {
  Handoff h = pullHandoff();
  encode(h);
  Handoff out;
  TEST_ASSERT_TRUE(decode(&h, sizeof(h), out) == Verdict::Ok);
  TEST_ASSERT_EQUAL_UINT8(uint8_t(Mode::Pull), out.mode);
  TEST_ASSERT_EQUAL_STRING("http://10.0.0.5:5000/update?dev=gw&ver=1.2", out.url);
  TEST_ASSERT_EQUAL_STRING("tok-123", out.token);
  TEST_ASSERT_EQUAL_UINT32(1250000, out.size);
  TEST_ASSERT_EQUAL_UINT8(71, out.sigLen);
  TEST_ASSERT_EQUAL_UINT8(0xA0 + 70, out.sig[70]);
  TEST_ASSERT_EQUAL_UINT8(32, out.sha256[31]);
  TEST_ASSERT_EQUAL_STRING("1.3", out.targetVersion);
  TEST_ASSERT_EQUAL_STRING("site-ap", out.wifiSsid);
  TEST_ASSERT_EQUAL_UINT8(5, out.attemptsAllowed);
  TEST_ASSERT_TRUE(hasSha256(out));
  TEST_ASSERT_EQUAL_UINT16(2, out.version);
  TEST_ASSERT_TRUE(hasAdmin(out));
  TEST_ASSERT_EQUAL_UINT8(uint8_t(AdminKdf::Pbkdf2Sha256), out.adminKdf);
  TEST_ASSERT_EQUAL_UINT8(16, out.adminSaltLen);
  TEST_ASSERT_EQUAL_UINT32(2000, out.adminIter);
  TEST_ASSERT_EQUAL_UINT8(0x1F, out.adminSalt[15]);
  TEST_ASSERT_EQUAL_UINT8(0xC0 ^ 31, out.adminKey[31]);
  TEST_ASSERT_EQUAL_STRING("admin", out.adminUser);
  TEST_ASSERT_EQUAL_STRING("k7mq-4hzt-p9xw", out.apPass);
  TEST_ASSERT_EQUAL_STRING("gw-404cca4798f4-1a2b3c4d", out.deviceId);
}

void test_v1_handoff_is_read_as_v2() {
  const std::vector<uint8_t> blob = v1Blob(kSha256Admin);
  TEST_ASSERT_EQUAL_UINT(616, blob.size());
  Handoff out;
  memset(&out, 0x77, sizeof(out));
  TEST_ASSERT_TRUE(decode(blob.data(), blob.size(), out) == Verdict::Ok);
  TEST_ASSERT_EQUAL_UINT16(kHandoffVersion, out.version);
  TEST_ASSERT_EQUAL_UINT8(uint8_t(Mode::Push), out.mode);
  TEST_ASSERT_EQUAL_UINT8(3, out.attemptsAllowed);
  TEST_ASSERT_EQUAL_STRING("http://h/x.bin", out.url);
  TEST_ASSERT_EQUAL_STRING("tok", out.token);
  TEST_ASSERT_EQUAL_UINT32(4096, out.size);
  TEST_ASSERT_EQUAL_UINT8(0xAB, out.sha256[0]);
  TEST_ASSERT_EQUAL_UINT8(70, out.sigLen);
  TEST_ASSERT_EQUAL_UINT8(0x5A, out.sig[69]);
  TEST_ASSERT_EQUAL_STRING("1.3", out.targetVersion);
  TEST_ASSERT_EQUAL_STRING("secret", out.wifiPass);
  // Its password hash: an SHA-256 record; the version 2 fields empty.
  TEST_ASSERT_EQUAL_UINT8(uint8_t(AdminKdf::Sha256), out.adminKdf);
  TEST_ASSERT_EQUAL_UINT8(0x8c, out.adminKey[0]);
  TEST_ASSERT_EQUAL_UINT8(0x18, out.adminKey[31]);
  TEST_ASSERT_EQUAL_UINT8(0, out.adminSaltLen);
  TEST_ASSERT_EQUAL_UINT32(0, out.adminIter);
  TEST_ASSERT_EQUAL_STRING("", out.adminUser);
  TEST_ASSERT_EQUAL_STRING("", out.apPass);
  TEST_ASSERT_EQUAL_STRING("", out.deviceId);
  // Sealed again it is a version 2 record of the same content.
  Handoff again = out;
  encode(again);
  Handoff back;
  TEST_ASSERT_TRUE(decode(&again, sizeof(again), back) == Verdict::Ok);
  TEST_ASSERT_EQUAL_INT(0, memcmp(&again, &back, sizeof(back)));

  // No password (or not 64 hex digits): no check.
  TEST_ASSERT_TRUE(decode(v1Blob("").data(), 616, out) == Verdict::Ok);
  TEST_ASSERT_FALSE(hasAdmin(out));
  TEST_ASSERT_TRUE(decode(v1Blob("8c69").data(), 616, out) == Verdict::Ok);
  TEST_ASSERT_FALSE(hasAdmin(out));
  // A v1 record is checked as one: its CRC, its size.
  std::vector<uint8_t> bad = blob;
  bad[300] ^= 1;
  TEST_ASSERT_TRUE(decode(bad.data(), bad.size(), out) == Verdict::BadCrc);
  TEST_ASSERT_TRUE(decode(blob.data(), blob.size() - 4, out) == Verdict::BadSize);
  std::vector<uint8_t> longer = blob;
  longer.resize(sizeof(Handoff), 0);
  TEST_ASSERT_TRUE(decode(longer.data(), longer.size(), out) == Verdict::BadSize);
}

void test_out_of_range_fields_are_cleared() {
  // encode() clears what decode() would: the stored record is always sane.
  Handoff h = pullHandoff();
  h.sigLen = kSigMax + 1;
  h.adminIter = kAdminIterMax + 1;
  setText(h.apPass, "short");
  encode(h);
  TEST_ASSERT_EQUAL_UINT8(0, h.sigLen);
  TEST_ASSERT_FALSE(hasAdmin(h));
  TEST_ASSERT_EQUAL_UINT8(0, h.adminSaltLen);
  TEST_ASSERT_EQUAL_UINT8(0, h.adminKey[5]);
  TEST_ASSERT_EQUAL_STRING("", h.apPass);

  // A writer that sealed them anyway (CRC good): decode() clears them.
  struct Case {
    uint8_t kdf, saltLen;
    uint32_t iter;
    bool admin;
  };
  const Case cases[] = {
      {uint8_t(AdminKdf::Pbkdf2Sha256), 16, 2000, true},
      {uint8_t(AdminKdf::Pbkdf2Sha256), 0, 2000, false},
      {uint8_t(AdminKdf::Pbkdf2Sha256), kAdminSaltMax + 1, 2000, false},
      {uint8_t(AdminKdf::Pbkdf2Sha256), 16, 0, false},
      {uint8_t(AdminKdf::Pbkdf2Sha256), 16, kAdminIterMax, true},
      {uint8_t(AdminKdf::Sha256), 0, 0, true},
      {7, 16, 2000, false},
  };
  for (const Case& c : cases) {
    Handoff r = pullHandoff();
    encode(r);
    r.adminKdf = c.kdf;
    r.adminSaltLen = c.saltLen;
    r.adminIter = c.iter;
    r.crc = crc32Ieee(&r, offsetof(Handoff, crc));
    Handoff out;
    TEST_ASSERT_TRUE(decode(&r, sizeof(r), out) == Verdict::Ok);
    TEST_ASSERT_EQUAL(c.admin, hasAdmin(out));
    if (!c.admin) TEST_ASSERT_EQUAL_UINT32(0, out.adminIter);
  }
  for (const char* pass : {"1234567", "pässwort1", "tab\there!"}) {
    Handoff r = pullHandoff();
    encode(r);
    setText(r.apPass, pass);
    r.crc = crc32Ieee(&r, offsetof(Handoff, crc));
    Handoff out;
    TEST_ASSERT_TRUE(decode(&r, sizeof(r), out) == Verdict::Ok);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", out.apPass, pass);
  }
}

void test_ap_pass_rules() {
  TEST_ASSERT_TRUE(apPassOk("12345678"));
  TEST_ASSERT_TRUE(apPassOk(std::string(63, 'x').c_str()));
  TEST_ASSERT_TRUE(apPassOk("with space ~!"));
  TEST_ASSERT_FALSE(apPassOk("1234567"));
  TEST_ASSERT_FALSE(apPassOk(std::string(64, 'x').c_str()));
  TEST_ASSERT_FALSE(apPassOk("caf\xc3\xa9-1234"));
  TEST_ASSERT_FALSE(apPassOk(""));
  TEST_ASSERT_FALSE(apPassOk(nullptr));
}

void test_result_round_trip_and_describe() {
  Result r;
  clear(r, Outcome::Ok);
  setText(r.from, "1.2");
  setText(r.to, "1.3");
  r.attempts = 1;
  r.bytes = 1250000;
  r.durationMs = 41700;
  encode(r);
  Result out;
  TEST_ASSERT_TRUE(decode(&r, sizeof(r), out) == Verdict::Ok);
  char line[96];
  describe(out, line, sizeof(line));
  TEST_ASSERT_EQUAL_STRING("ok: 1.2 -> 1.3, 1 attempt, 1250000 B in 42 s", line);

  clear(r, Outcome::FailedBeforeWrite);
  setText(r.from, "1.2");
  setText(r.lastError, "HTTP 404");
  r.attempts = 2;
  encode(r);
  TEST_ASSERT_TRUE(decode(&r, sizeof(r), out) == Verdict::Ok);
  describe(out, line, sizeof(line));
  TEST_ASSERT_EQUAL_STRING("failed-before-write: HTTP 404 (1.2 kept, 2 attempts)", line);
  // Cut to the buffer, always terminated.
  char small[8];
  TEST_ASSERT_EQUAL_UINT(7, describe(out, small, sizeof(small)));
  TEST_ASSERT_EQUAL_STRING("failed-", small);
}

void test_crc_catches_any_flipped_byte() {
  Handoff h = pullHandoff();
  encode(h);
  std::vector<uint8_t> blob(reinterpret_cast<uint8_t*>(&h), reinterpret_cast<uint8_t*>(&h) + sizeof(h));
  Handoff out;
  for (size_t i = 6; i < blob.size(); ++i) {  // past magic and version
    std::vector<uint8_t> bad = blob;
    bad[i] ^= 0x40;
    TEST_ASSERT_TRUE_MESSAGE(decode(bad.data(), bad.size(), out) == Verdict::BadCrc, std::to_string(i).c_str());
  }
}

void test_padding_is_zeroed_so_the_blob_is_stable() {
  // Garbage in the padding (e.g. a stack struct): encode() zeroes it, so
  // two seals of the same content are the same bytes and the same CRC.
  Handoff a = pullHandoff();
  Handoff b = pullHandoff();
  memset(reinterpret_cast<uint8_t*>(&b) + offsetof(Handoff, adminSaltLen) + 1, 0xEE,
         offsetof(Handoff, adminIter) - offsetof(Handoff, adminSaltLen) - 1);
  encode(a);
  encode(b);
  TEST_ASSERT_EQUAL_INT(0, memcmp(&a, &b, sizeof(a)));
  Result r1, r2;
  clear(r1, Outcome::Aborted);
  memset(&r2, 0x5A, sizeof(r2));
  clear(r2, Outcome::Aborted);  // clear() zeroes padding too
  memset(reinterpret_cast<uint8_t*>(&r2) + offsetof(Result, attempts) + 2, 0x77, 2);
  encode(r1);
  encode(r2);
  TEST_ASSERT_EQUAL_INT(0, memcmp(&r1, &r2, sizeof(r1)));
}

void test_truncated_and_foreign_blobs() {
  Handoff h = pullHandoff();
  encode(h);
  Handoff out;
  TEST_ASSERT_TRUE(decode(&h, sizeof(h) - 1, out) == Verdict::BadSize);
  TEST_ASSERT_TRUE(decode(&h, 5, out) == Verdict::BadSize);
  TEST_ASSERT_TRUE(decode(nullptr, sizeof(h), out) == Verdict::BadSize);
  std::vector<uint8_t> longer(sizeof(h) + 4, 0);
  memcpy(longer.data(), &h, sizeof(h));
  TEST_ASSERT_TRUE(decode(longer.data(), longer.size(), out) == Verdict::BadSize);
  // A result blob is no handoff, and the other way round.
  Result r;
  clear(r, Outcome::Ok);
  encode(r);
  TEST_ASSERT_TRUE(decode(&r, sizeof(r), out) == Verdict::BadMagic);
  Result rout;
  TEST_ASSERT_TRUE(decode(&h, sizeof(h), rout) == Verdict::BadMagic);
  // An erased / zero blob.
  std::vector<uint8_t> zero(sizeof(h), 0);
  TEST_ASSERT_TRUE(decode(zero.data(), zero.size(), out) == Verdict::BadMagic);
}

void test_other_version_is_refused_whatever_its_size() {
  Handoff h = pullHandoff();
  encode(h);
  h.version = kHandoffVersion + 1;  // a newer layout, resealed by its writer
  h.crc = crc32Ieee(&h, offsetof(Handoff, crc));
  Handoff out;
  TEST_ASSERT_TRUE(decode(&h, sizeof(h), out) == Verdict::OtherVersion);
  // A larger v3 record is still "another version", not a bad size.
  std::vector<uint8_t> v3(sizeof(h) + 40, 0);
  memcpy(v3.data(), &h, sizeof(h));
  TEST_ASSERT_TRUE(decode(v3.data(), v3.size(), out) == Verdict::OtherVersion);
  h.version = 0;
  TEST_ASSERT_TRUE(decode(&h, sizeof(h), out) == Verdict::OtherVersion);
  Result r;
  clear(r, Outcome::Ok);
  encode(r);
  r.version = 0;
  Result rout;
  TEST_ASSERT_TRUE(decode(&r, sizeof(r), rout) == Verdict::OtherVersion);
}

void test_strings_are_cut_and_terminated() {
  Handoff h;
  clear(h, Mode::Push);
  std::string longUrl(300, 'u');
  TEST_ASSERT_FALSE(setText(h.url, longUrl.c_str()));
  TEST_ASSERT_EQUAL_UINT(sizeof(h.url) - 1, strlen(h.url));
  TEST_ASSERT_TRUE(setText(h.wifiSsid, std::string(32, 's').c_str()));  // 32: the longest SSID fits
  TEST_ASSERT_TRUE(setText(h.apPass, std::string(63, 'a').c_str()));
  TEST_ASSERT_FALSE(setText(h.adminUser, std::string(32, 'u').c_str()));
  TEST_ASSERT_EQUAL_UINT(31, strlen(h.adminUser));
  TEST_ASSERT_TRUE(setText(h.token, nullptr));
  TEST_ASSERT_EQUAL_STRING("", h.token);
  // A writer that left a field unterminated: decode() terminates the copy.
  encode(h);
  memset(h.targetVersion, 'v', sizeof(h.targetVersion));
  h.crc = crc32Ieee(&h, offsetof(Handoff, crc));
  Handoff out;
  TEST_ASSERT_TRUE(decode(&h, sizeof(h), out) == Verdict::Ok);
  TEST_ASSERT_EQUAL_UINT(sizeof(out.targetVersion) - 1, strlen(out.targetVersion));
}

void test_hex_helpers() {
  uint8_t b[4];
  TEST_ASSERT_TRUE(fromHex("00fFa5C3", b, 4));
  TEST_ASSERT_EQUAL_UINT8(0xFF, b[1]);
  TEST_ASSERT_EQUAL_UINT8(0xC3, b[3]);
  TEST_ASSERT_FALSE(fromHex("00ff", b, 4));
  TEST_ASSERT_FALSE(fromHex("00fg0000", b, 4));
  char hex[9];
  toHex(b, 4, hex);
  TEST_ASSERT_EQUAL_STRING("00ffa5c3", hex);
  uint8_t der[kSigMax];
  uint8_t len = 0;
  TEST_ASSERT_TRUE(sigFromHex("3045022100", der, len));
  TEST_ASSERT_EQUAL_UINT8(5, len);
  TEST_ASSERT_FALSE(sigFromHex("", der, len));
  TEST_ASSERT_FALSE(sigFromHex("304", der, len));
  TEST_ASSERT_FALSE(sigFromHex(std::string(2 * kSigMax + 2, 'a').c_str(), der, len));
  Handoff h;
  clear(h, Mode::Pull);
  TEST_ASSERT_FALSE(hasSha256(h));
}

void test_trailer_round_trip() {
  uint8_t der[71];
  for (size_t i = 0; i < sizeof(der); ++i) der[i] = uint8_t(i * 3);
  uint8_t t[kTrailerSize];
  TEST_ASSERT_TRUE(encodeTrailer(4096, der, sizeof(der), t));
  TEST_ASSERT_EQUAL_INT(0, memcmp(t, "ESRKSIG1", 8));
  SigTrailer s;
  TEST_ASSERT_TRUE(parseTrailer(t, 4096 + kTrailerSize, s) == TrailerVerdict::Ok);
  TEST_ASSERT_EQUAL_UINT32(4096, s.imageLen);
  TEST_ASSERT_EQUAL_UINT8(71, s.sigLen);
  TEST_ASSERT_EQUAL_INT(0, memcmp(s.der, der, sizeof(der)));
  // The wire layout (what tools/ writes): imageLen LE at 8, sigLen at 12,
  // DER at 13, zero pad, CRC LE at 124 over 0..123.
  TEST_ASSERT_EQUAL_UINT8(0x00, t[8]);
  TEST_ASSERT_EQUAL_UINT8(0x10, t[9]);
  TEST_ASSERT_EQUAL_UINT8(71, t[12]);
  TEST_ASSERT_EQUAL_UINT8(der[0], t[13]);
  for (size_t i = 13 + 71; i < 124; ++i) TEST_ASSERT_EQUAL_UINT8(0, t[i]);
  const uint32_t crc = crc32Ieee(t, 124);
  TEST_ASSERT_EQUAL_UINT8(crc & 0xFF, t[124]);
  TEST_ASSERT_EQUAL_UINT8(crc >> 24, t[127]);
  TEST_ASSERT_FALSE(encodeTrailer(10, der, 0, t));
  TEST_ASSERT_FALSE(encodeTrailer(10, der, kSigMax + 1, t));
}

void test_trailer_refusals() {
  uint8_t der[70] = {0x30};
  uint8_t t[kTrailerSize];
  TEST_ASSERT_TRUE(encodeTrailer(1000, der, sizeof(der), t));
  SigTrailer s;
  // Not for this file's length (truncated or extended download).
  TEST_ASSERT_TRUE(parseTrailer(t, 999 + kTrailerSize, s) == TrailerVerdict::Corrupt);
  TEST_ASSERT_TRUE(parseTrailer(t, 1001 + kTrailerSize, s) == TrailerVerdict::Corrupt);
  // A flipped bit anywhere.
  for (size_t i = 8; i < kTrailerSize; ++i) {
    uint8_t bad[kTrailerSize];
    memcpy(bad, t, sizeof(bad));
    bad[i] ^= 0x01;
    TEST_ASSERT_TRUE_MESSAGE(parseTrailer(bad, 1000 + kTrailerSize, s) == TrailerVerdict::Corrupt,
                             std::to_string(i).c_str());
  }
  // No magic: an unsigned image (its last bytes are image).
  uint8_t plain[kTrailerSize] = {0xE9};
  TEST_ASSERT_TRUE(parseTrailer(plain, 5000, s) == TrailerVerdict::None);
  TEST_ASSERT_TRUE(parseTrailer(t, kTrailerSize - 1, s) == TrailerVerdict::None);
  // sigLen out of range, though the CRC is right.
  uint8_t odd[kTrailerSize];
  memcpy(odd, t, sizeof(odd));
  odd[12] = kSigMax + 1;
  const uint32_t crc = crc32Ieee(odd, 124);
  memcpy(odd + 124, &crc, 4);
  TEST_ASSERT_TRUE(parseTrailer(odd, 1000 + kTrailerSize, s) == TrailerVerdict::Corrupt);
}

// A signed file streamed in uneven chunks: exactly the image passes, the
// trailer stays held; an unsigned file: all but its last 128 bytes pass.
void test_tail_hold_streams_the_image_and_keeps_the_trailer() {
  const size_t imageLen = 3 * 4096 + 77;
  std::vector<uint8_t> file(imageLen);
  for (size_t i = 0; i < imageLen; ++i) file[i] = uint8_t(i * 7 + 1);
  uint8_t der[72] = {0x30, 0x46};
  file.resize(imageLen + kTrailerSize);
  TEST_ASSERT_TRUE(encodeTrailer(uint32_t(imageLen), der, sizeof(der), file.data() + imageLen));

  const size_t chunks[] = {1, 127, 128, 1, 4096, 3000, 5, 200, 100000};
  for (size_t first : {size_t(0), size_t(1), size_t(127), size_t(128), size_t(129)}) {
    TailHold hold;
    std::vector<uint8_t> passed;
    auto sink = [&](const uint8_t* p, size_t n) {
      passed.insert(passed.end(), p, p + n);
      return true;
    };
    size_t at = 0;
    if (first) {
      TEST_ASSERT_TRUE(hold.feed(file.data(), first, sink));
      at = first;
    }
    for (size_t i = 0; at < file.size(); i = (i + 1) % (sizeof(chunks) / sizeof(chunks[0]))) {
      const size_t n = std::min(chunks[i], file.size() - at);
      TEST_ASSERT_TRUE(hold.feed(file.data() + at, n, sink));
      at += n;
    }
    TEST_ASSERT_EQUAL_UINT(kTrailerSize, hold.held());
    TEST_ASSERT_EQUAL_UINT(imageLen, passed.size());
    TEST_ASSERT_EQUAL_INT(0, memcmp(passed.data(), file.data(), imageLen));
    SigTrailer s;
    TEST_ASSERT_TRUE(parseTrailer(hold.tail(), uint32_t(file.size()), s) == TrailerVerdict::Ok);
    TEST_ASSERT_EQUAL_UINT32(imageLen, s.imageLen);
  }

  // Shorter than a trailer: everything held, nothing passed.
  TailHold small;
  size_t passedN = 0;
  auto count = [&](const uint8_t*, size_t n) {
    passedN += n;
    return true;
  };
  TEST_ASSERT_TRUE(small.feed(file.data(), 100, count));
  TEST_ASSERT_EQUAL_UINT(0, passedN);
  TEST_ASSERT_EQUAL_UINT(100, small.held());
  // A sink that refuses stops the feed.
  TailHold stop;
  TEST_ASSERT_TRUE(stop.feed(file.data(), 128, count));
  TEST_ASSERT_FALSE(stop.feed(file.data(), 10, [](const uint8_t*, size_t) { return false; }));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_handoff_round_trip);
  RUN_TEST(test_v1_handoff_is_read_as_v2);
  RUN_TEST(test_out_of_range_fields_are_cleared);
  RUN_TEST(test_ap_pass_rules);
  RUN_TEST(test_result_round_trip_and_describe);
  RUN_TEST(test_crc_catches_any_flipped_byte);
  RUN_TEST(test_padding_is_zeroed_so_the_blob_is_stable);
  RUN_TEST(test_truncated_and_foreign_blobs);
  RUN_TEST(test_other_version_is_refused_whatever_its_size);
  RUN_TEST(test_strings_are_cut_and_terminated);
  RUN_TEST(test_hex_helpers);
  RUN_TEST(test_trailer_round_trip);
  RUN_TEST(test_trailer_refusals);
  RUN_TEST(test_tail_hold_streams_the_image_and_keeps_the_trailer);
  return UNITY_END();
}
