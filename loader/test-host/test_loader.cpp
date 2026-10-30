// Host tests of the loader's IDF-free parts: the image head check, the
// announced size, the retry pauses, the HTTP download's URL / head / chunked parsing, the
// signature check against vectors from tools/ota_sign.py and Python's
// cryptography, and the upload page's password check (PBKDF2 against
// RFC 7914 and Python's hashlib; the same mbedtls calls as on the chip).

#include <unity.h>

#include <string.h>

#include <string>
#include <vector>

#include "http_lite.h"
#include "image_rules.h"
#include "sig_verify.h"
#include "system/OtaHandoff.h"

using namespace loader;

void setUp() {}
void tearDown() {}

namespace {

// The test image: 5000 bytes (i * 7 + 3) & 0xff, as the vectors signed it.
std::vector<uint8_t> pattern() {
  std::vector<uint8_t> v(5000);
  for (size_t i = 0; i < v.size(); ++i) v[i] = uint8_t((i * 7 + 3) & 0xff);
  return v;
}

// tools/ota_sign.py sign --force of pattern(), with a throwaway key: its
// trailer and the key's public point (export-pubkey).
const uint8_t kToolTrailer[128] = {
    0x45, 0x53, 0x52, 0x4b, 0x53, 0x49, 0x47, 0x31, 0x88, 0x13, 0x00, 0x00, 0x46, 0x30, 0x44, 0x02,
    0x20, 0x57, 0xdb, 0x7b, 0x6f, 0x53, 0xdd, 0x69, 0x20, 0xfe, 0xc5, 0xc0, 0x9c, 0x4c, 0x0b, 0x88,
    0xbd, 0x01, 0xba, 0x95, 0x18, 0x94, 0x77, 0x81, 0x7c, 0x57, 0x4b, 0x37, 0xd6, 0xa9, 0xd8, 0x3e,
    0x98, 0x02, 0x20, 0x07, 0xea, 0xb8, 0xc1, 0x6a, 0x57, 0x12, 0x20, 0x33, 0x28, 0x42, 0xc3, 0x06,
    0x9a, 0xfb, 0xeb, 0xf9, 0x50, 0x9c, 0x58, 0x8e, 0xfd, 0x38, 0x28, 0x72, 0xc7, 0x2e, 0x6e, 0xfa,
    0x9b, 0x1d, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa5, 0xe8, 0x10, 0xa5};
const uint8_t kToolPub[65] = {
    0x04, 0xea, 0x20, 0xa9, 0x7c, 0xe5, 0x2e, 0x84, 0x6c, 0x27, 0xa9, 0xa9, 0x80, 0xb0, 0xb3, 0x15, 0x51,
    0x44, 0x91, 0xa4, 0x50, 0xe8, 0xea, 0x48, 0x6a, 0xcc, 0xdf, 0xad, 0x3c, 0xdd, 0x90, 0xe0, 0x02, 0xa5,
    0xd5, 0x97, 0x65, 0xda, 0x07, 0xa6, 0x04, 0x29, 0xde, 0xe1, 0x28, 0x16, 0xdf, 0xce, 0xe0, 0xf1, 0x34,
    0x66, 0xae, 0x53, 0x0c, 0xa5, 0x6c, 0x97, 0x16, 0x06, 0xe4, 0x1e, 0x5e, 0x01, 0x95};

// cryptography's ECDSA(SHA256) of pattern() with another key (a 72-byte DER).
const uint8_t kPyPub[65] = {
    0x04, 0x69, 0x0b, 0xae, 0x8a, 0xd0, 0x50, 0xa7, 0x7c, 0x5e, 0x75, 0x5b, 0x7a, 0xf6, 0xed, 0x76, 0x1b,
    0x1b, 0xb4, 0x0d, 0x04, 0xc3, 0xb3, 0x5c, 0x6d, 0x0e, 0x33, 0x4a, 0x75, 0xa5, 0x0e, 0xdf, 0x04, 0xd0,
    0xc4, 0x29, 0x32, 0x4f, 0x56, 0x50, 0x47, 0xce, 0x70, 0x55, 0x83, 0xd4, 0x57, 0x7d, 0x34, 0x77, 0xde,
    0xe0, 0xc9, 0x87, 0x40, 0x4a, 0xa8, 0x0c, 0x3f, 0xc6, 0x95, 0x28, 0x4c, 0x3d, 0x11};
const uint8_t kPySig[72] = {
    0x30, 0x46, 0x02, 0x21, 0x00, 0xed, 0xaa, 0x2b, 0x8e, 0xc0, 0x42, 0xa2, 0x4a, 0x76, 0x3a, 0xb6, 0x0d, 0xb9,
    0x43, 0xb0, 0x9a, 0xa4, 0x7d, 0x4f, 0xfc, 0x8d, 0x04, 0xca, 0xa7, 0xe1, 0x02, 0xdf, 0xae, 0xff, 0xf1, 0x4e,
    0x5c, 0x02, 0x21, 0x00, 0xcf, 0xd8, 0x13, 0x65, 0x2b, 0x26, 0x59, 0x1b, 0x73, 0x2d, 0xb7, 0x49, 0x3f, 0xbe,
    0x78, 0xe0, 0x30, 0xe7, 0x41, 0x50, 0x3f, 0x62, 0x05, 0x2c, 0x2d, 0x1c, 0xa5, 0x46, 0xb0, 0x9d, 0xe7, 0x53};
const char kPatternSha[] = "34398b85297bf7d9dfb59b8d511d8bbb44ab23e891570e4395e7871475fc8afb";

// An app image's first bytes: header, segment header, app description.
std::vector<uint8_t> head(uint16_t chip, const char* version, const char* project, bool appDesc = true) {
  std::vector<uint8_t> v(256, 0);
  v[0] = kImageMagic;
  v[1] = 3;  // segments
  v[12] = uint8_t(chip);
  v[13] = uint8_t(chip >> 8);
  if (appDesc) {
    const uint32_t m = kAppDescMagic;
    memcpy(v.data() + kAppDescOffset, &m, 4);
    memcpy(v.data() + kAppDescVersion, version, strlen(version));
    memcpy(v.data() + kAppDescProject, project, strlen(project));
  }
  return v;
}

}  // namespace

// ---- image head -----------------------------------------------------------

void test_head_ok_reads_version() {
  const auto v = head(5, "1.2.3", "rs485-c3");
  HeadInfo info;
  TEST_ASSERT_EQUAL(int(Head::Ok), int(checkHead(v.data(), v.size(), 5, info)));
  TEST_ASSERT_EQUAL_STRING("1.2.3", info.version);
  TEST_ASSERT_EQUAL_STRING("rs485-c3", info.project);
}

void test_head_refusals() {
  HeadInfo info;
  auto v = head(5, "1", "app");
  TEST_ASSERT_EQUAL(int(Head::TooShort), int(checkHead(v.data(), kHeadNeeded - 1, 5, info)));
  TEST_ASSERT_EQUAL(int(Head::WrongChip), int(checkHead(v.data(), v.size(), 13, info)));  // a C6 loader
  v[0] = 0xE8;
  TEST_ASSERT_EQUAL(int(Head::BadMagic), int(checkHead(v.data(), v.size(), 5, info)));
  v = head(5, "1", "app", false);
  TEST_ASSERT_EQUAL(int(Head::NoAppDesc), int(checkHead(v.data(), v.size(), 5, info)));
  v = head(5, "0.1.0", kLoaderProject);
  TEST_ASSERT_EQUAL(int(Head::IsLoader), int(checkHead(v.data(), v.size(), 5, info)));
}

void test_head_long_strings_terminated() {
  auto v = head(0, "", "");
  memset(v.data() + kAppDescVersion, 'v', 32);  // no NUL in the field
  HeadInfo info;
  TEST_ASSERT_EQUAL(int(Head::Ok), int(checkHead(v.data(), v.size(), 0, info)));
  TEST_ASSERT_EQUAL(31, int(strlen(info.version)));
}

// ---- retries --------------------------------------------------------------

void test_retry_pauses() {
  TEST_ASSERT_EQUAL_UINT32(10000, retryPauseMs(1));
  TEST_ASSERT_EQUAL_UINT32(30000, retryPauseMs(2));
  TEST_ASSERT_EQUAL_UINT32(120000, retryPauseMs(3));
  TEST_ASSERT_EQUAL_UINT32(300000, retryPauseMs(4));
  TEST_ASSERT_EQUAL_UINT32(300000, retryPauseMs(60));
  TEST_ASSERT_EQUAL(kDefaultAttempts, attemptsAllowed(0));
  TEST_ASSERT_EQUAL(3, attemptsAllowed(3));
}

// ---- The upload page's password -------------------------------------------

void hexTo(const char* hex, uint8_t* out, size_t n) { TEST_ASSERT_TRUE(ESPRack::ota::fromHex(hex, out, n)); }

void test_pbkdf2_vectors() {
  uint8_t key[32], want[32];
  // RFC 7914 §11: PBKDF2-HMAC-SHA256("passwd", "salt", 1), its first 32 bytes.
  TEST_ASSERT_TRUE(pbkdf2Sha256("passwd", 6, reinterpret_cast<const uint8_t*>("salt"), 4, 1, key, sizeof(key)));
  hexTo("55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc", want, 32);
  TEST_ASSERT_EQUAL_INT(0, memcmp(key, want, 32));
  // The app's format (SecurityHash.h: 16-byte salt, 2000 rounds), hashlib.pbkdf2_hmac.
  uint8_t salt[16];
  for (size_t i = 0; i < sizeof(salt); ++i) salt[i] = uint8_t(0x10 + i);
  TEST_ASSERT_TRUE(pbkdf2Sha256("admin", 5, salt, sizeof(salt), 2000, key, sizeof(key)));
  hexTo("7158b7b47d51e8b1f7bf0a4f7217ee297fc5ed5f5c2f94ac73f983c49abaa957", want, 32);
  TEST_ASSERT_EQUAL_INT(0, memcmp(key, want, 32));
}

void test_admin_password_check() {
  namespace ota = ESPRack::ota;
  // A version 2 handoff: the stored PBKDF2 record of user "admin" (UTF-8 password).
  ota::Handoff h;
  ota::clear(h, ota::Mode::Push);
  h.adminKdf = uint8_t(ota::AdminKdf::Pbkdf2Sha256);
  h.adminSaltLen = 16;
  h.adminIter = 2000;
  for (size_t i = 0; i < 16; ++i) h.adminSalt[i] = uint8_t(0x10 + i);
  hexTo("168931b295c3b9b3ef14eeabf67097f90870722ac5098df7c02cc16966573de7", h.adminKey, 32);
  const char* pw = "p\xc3\xa4ssw\xc3\xb6rd";  // "pässwörd"
  TEST_ASSERT_TRUE(adminPasswordOk(h, pw, strlen(pw)));
  TEST_ASSERT_FALSE(adminPasswordOk(h, "passwrd", 7));
  TEST_ASSERT_FALSE(adminPasswordOk(h, "", 0));
  h.adminIter = 1999;  // the record's own rounds, not a fixed count
  TEST_ASSERT_FALSE(adminPasswordOk(h, pw, strlen(pw)));
  h.adminIter = ota::kAdminIterMax + 1;  // out of range: never checked
  TEST_ASSERT_FALSE(adminPasswordOk(h, pw, strlen(pw)));
  // The app's password "admin" in its format.
  h.adminIter = 2000;
  hexTo("7158b7b47d51e8b1f7bf0a4f7217ee297fc5ed5f5c2f94ac73f983c49abaa957", h.adminKey, 32);
  TEST_ASSERT_TRUE(adminPasswordOk(h, "admin", 5));
  TEST_ASSERT_FALSE(adminPasswordOk(h, "Admin", 5));
  // A version 1 handoff: SHA-256("admin"), decoded into the same record.
  h.adminKdf = uint8_t(ota::AdminKdf::Sha256);
  h.adminSaltLen = 0;
  h.adminIter = 0;
  hexTo("8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918", h.adminKey, 32);
  TEST_ASSERT_TRUE(adminPasswordOk(h, "admin", 5));
  TEST_ASSERT_FALSE(adminPasswordOk(h, "admin ", 6));
  // No record: no password matches (the page is open instead: loader_web).
  h.adminKdf = uint8_t(ota::AdminKdf::None);
  TEST_ASSERT_FALSE(adminPasswordOk(h, "admin", 5));
}

// ---- URL ------------------------------------------------------------------

void test_url_forms() {
  http::Url u;
  TEST_ASSERT_TRUE(http::parseUrl("http://10.0.0.5:8080/fw/app.bin?t=1#x", u));
  TEST_ASSERT_EQUAL_STRING("10.0.0.5", u.host);
  TEST_ASSERT_EQUAL(8080, u.port);
  TEST_ASSERT_EQUAL_STRING("/fw/app.bin?t=1", u.path);
  TEST_ASSERT_TRUE(http::parseUrl("HTTP://mship.local", u));
  TEST_ASSERT_EQUAL_STRING("mship.local", u.host);
  TEST_ASSERT_EQUAL(80, u.port);
  TEST_ASSERT_EQUAL_STRING("/", u.path);
  TEST_ASSERT_TRUE(http::parseUrl("http://h?q=1", u));
  TEST_ASSERT_EQUAL_STRING("/?q=1", u.path);
}

void test_url_refusals() {
  http::Url u;
  TEST_ASSERT_FALSE(http::parseUrl("https://h/x", u));
  TEST_ASSERT_FALSE(http::parseUrl("ftp://h/x", u));
  TEST_ASSERT_FALSE(http::parseUrl("http:///x", u));
  TEST_ASSERT_FALSE(http::parseUrl("http://h:0/x", u));
  TEST_ASSERT_FALSE(http::parseUrl("http://h:65536/x", u));
  TEST_ASSERT_FALSE(http::parseUrl("http://h:/x", u));
  TEST_ASSERT_FALSE(http::parseUrl(("http://" + std::string(64, 'a') + "/").c_str(), u));
  TEST_ASSERT_FALSE(http::parseUrl(("http://h/" + std::string(260, 'p')).c_str(), u));
}

void test_redirects() {
  http::Url from, to;
  TEST_ASSERT_TRUE(http::parseUrl("http://a:81/x", from));
  TEST_ASSERT_TRUE(http::redirect(from, "/y/z.bin", to));
  TEST_ASSERT_EQUAL_STRING("a", to.host);
  TEST_ASSERT_EQUAL(81, to.port);
  TEST_ASSERT_EQUAL_STRING("/y/z.bin", to.path);
  TEST_ASSERT_TRUE(http::redirect(from, "http://b/c", to));
  TEST_ASSERT_EQUAL_STRING("b", to.host);
  TEST_ASSERT_EQUAL(80, to.port);
  TEST_ASSERT_FALSE(http::redirect(from, "https://b/c", to));
  TEST_ASSERT_FALSE(http::redirect(from, "//b/c", to));
  TEST_ASSERT_FALSE(http::redirect(from, "c.bin", to));
}

// ---- response head --------------------------------------------------------

void test_head_parse_split_anywhere() {
  const std::string resp =
      "HTTP/1.1 200 OK\r\nServer: x\r\nContent-Length: 1234\r\nSet-Cookie: " + std::string(500, 'c') +
      "\r\nContent-Type: application/octet-stream\r\n\r\nBODY";
  for (size_t cut = 1; cut < resp.size(); cut += 7) {
    http::Head h;
    size_t used = h.feed(resp.data(), cut);
    if (!h.done()) used += h.feed(resp.data() + cut, resp.size() - cut);
    TEST_ASSERT_TRUE(h.done());
    TEST_ASSERT_FALSE(h.bad());
    TEST_ASSERT_EQUAL(200, h.status());
    TEST_ASSERT_EQUAL(1234, int(h.length()));
    TEST_ASSERT_FALSE(h.chunked());
    TEST_ASSERT_EQUAL(int(resp.size() - 4), int(used));  // "BODY" left over
  }
}

void test_head_redirect_and_chunked() {
  const char r[] = "HTTP/1.0 302 Found\r\nlocation: /next.bin\r\nTRANSFER-ENCODING: gzip, chunked\r\n\r\n";
  http::Head h;
  h.feed(r, sizeof(r) - 1);
  TEST_ASSERT_TRUE(h.done());
  TEST_ASSERT_EQUAL(302, h.status());
  TEST_ASSERT_EQUAL_STRING("/next.bin", h.location());
  TEST_ASSERT_TRUE(h.chunked());
  TEST_ASSERT_EQUAL(-1, int(h.length()));
}

void test_head_bad() {
  http::Head a;
  a.feed("SSH-2.0-OpenSSH\r\n\r\n", 19);
  TEST_ASSERT_TRUE(a.bad());
  http::Head b;
  const std::string big = "HTTP/1.1 200 OK\r\nX: " + std::string(http::Head::kMax, 'x');
  b.feed(big.data(), big.size());
  TEST_ASSERT_TRUE(b.bad());
}

// ---- chunked --------------------------------------------------------------

void test_chunked_any_split() {
  const std::string body = "4;ext=1\r\nWiki\r\n5\r\npedia\r\nE\r\n in\r\n\r\nchunks.\r\n0\r\nX-T: 1\r\n\r\n";
  for (size_t step = 1; step <= body.size(); ++step) {
    http::Chunked c;
    std::string in = body, out;
    for (size_t i = 0; i < in.size(); i += step) {
      const size_t n = in.size() - i < step ? in.size() - i : step;
      uint8_t* p = reinterpret_cast<uint8_t*>(&in[i]);
      const size_t k = c.feed(p, n, p);  // in place
      out.append(reinterpret_cast<char*>(p), k);
    }
    TEST_ASSERT_FALSE(c.bad());
    TEST_ASSERT_TRUE(c.end());
    TEST_ASSERT_EQUAL_STRING("Wikipedia in\r\n\r\nchunks.", out.c_str());
  }
}

void test_chunked_bad() {
  http::Chunked c;
  uint8_t in[] = "zz\r\n";
  c.feed(in, 4, in);
  TEST_ASSERT_TRUE(c.bad());
  http::Chunked d;
  uint8_t in2[] = "\r\n";
  d.feed(in2, 2, in2);
  TEST_ASSERT_TRUE(d.bad());
}

// ---- signatures -----------------------------------------------------------

void test_sha256_streaming() {
  const auto img = pattern();
  uint8_t one[32], two[32];
  char hex[65];
  sha256(img.data(), img.size(), one);
  ESPRack::ota::toHex(one, 32, hex);
  TEST_ASSERT_EQUAL_STRING(kPatternSha, hex);
  Sha256 s;
  for (size_t i = 0; i < img.size(); i += 4096) s.update(img.data() + i, img.size() - i < 4096 ? img.size() - i : 4096);
  s.finish(two);
  TEST_ASSERT_TRUE(memcmp(one, two, 32) == 0);
}

// The signing tool's trailer for the pattern image: it parses, and its
// signature verifies with the tool's key, not with another one.
void test_tool_signed_image_verifies() {
  const auto img = pattern();
  ESPRack::ota::SigTrailer t;
  TEST_ASSERT_EQUAL(int(ESPRack::ota::TrailerVerdict::Ok),
                    int(ESPRack::ota::parseTrailer(kToolTrailer, uint32_t(img.size() + 128), t)));
  TEST_ASSERT_EQUAL_UINT32(img.size(), t.imageLen);
  uint8_t sha[32];
  sha256(img.data(), img.size(), sha);
  TEST_ASSERT_EQUAL(int(Sig::Ok), int(verifyP256(kToolPub, sha, t.der, t.sigLen)));
  TEST_ASSERT_EQUAL(int(Sig::Bad), int(verifyP256(kPyPub, sha, t.der, t.sigLen)));
}

void test_python_signature_and_tampering() {
  const auto img = pattern();
  uint8_t sha[32];
  sha256(img.data(), img.size(), sha);
  TEST_ASSERT_EQUAL(int(Sig::Ok), int(verifyP256(kPyPub, sha, kPySig, sizeof(kPySig))));
  sha[5] ^= 1;  // another image
  TEST_ASSERT_EQUAL(int(Sig::Bad), int(verifyP256(kPyPub, sha, kPySig, sizeof(kPySig))));
  sha[5] ^= 1;
  uint8_t sig[72];
  memcpy(sig, kPySig, sizeof(sig));
  sig[40] ^= 0x10;  // s changed
  TEST_ASSERT_EQUAL(int(Sig::Bad), int(verifyP256(kPyPub, sha, sig, sizeof(sig))));
  TEST_ASSERT_EQUAL(int(Sig::Format), int(verifyP256(kPyPub, sha, kPySig, sizeof(kPySig) - 1)));  // cut
  TEST_ASSERT_EQUAL(int(Sig::Format), int(verifyP256(kPyPub, sha, nullptr, 0)));
  uint8_t pub[65];
  memcpy(pub, kPyPub, sizeof(pub));
  pub[64] ^= 1;  // not on the curve
  TEST_ASSERT_EQUAL(int(Sig::NoKey), int(verifyP256(pub, sha, kPySig, sizeof(kPySig))));
}

// An upload of image + trailer through the TailHold the writer uses: the
// image bytes reach the sink, the trailer doesn't, whatever the chunks.
void test_upload_split_keeps_trailer_out() {
  const auto img = pattern();
  std::vector<uint8_t> file(img);
  file.insert(file.end(), kToolTrailer, kToolTrailer + 128);
  for (size_t step : {1u, 100u, 127u, 128u, 129u, 1460u, 5128u}) {
    ESPRack::ota::TailHold hold;
    std::vector<uint8_t> got;
    for (size_t i = 0; i < file.size(); i += step) {
      const size_t n = file.size() - i < step ? file.size() - i : step;
      hold.feed(file.data() + i, n, [&](const uint8_t* p, size_t k) {
        got.insert(got.end(), p, p + k);
        return true;
      });
    }
    TEST_ASSERT_EQUAL(128, int(hold.held()));
    ESPRack::ota::SigTrailer t;
    TEST_ASSERT_EQUAL(int(ESPRack::ota::TrailerVerdict::Ok),
                      int(ESPRack::ota::parseTrailer(hold.tail(), uint32_t(file.size()), t)));
    TEST_ASSERT_TRUE(got == img);
  }
}

// A pull of a signed file whose handoff size is the file's length (an
// auto-update server's Content-Length): the image is the file less the
// trailer, and that size is as announced; so is the image's own length.
void test_signed_file_size_is_as_announced() {
  const auto img = pattern();
  std::vector<uint8_t> file(img);
  file.insert(file.end(), kToolTrailer, kToolTrailer + 128);
  ESPRack::ota::TailHold hold;
  uint32_t image = 0;
  for (size_t i = 0; i < file.size(); i += 1460) {
    const size_t n = file.size() - i < 1460 ? file.size() - i : 1460;
    hold.feed(file.data() + i, n, [&](const uint8_t*, size_t k) {
      image += uint32_t(k);
      return true;
    });
  }
  ESPRack::ota::SigTrailer t;
  const bool trailer = ESPRack::ota::parseTrailer(hold.tail(), uint32_t(file.size()), t) ==
                       ESPRack::ota::TrailerVerdict::Ok;
  TEST_ASSERT_TRUE(trailer);
  TEST_ASSERT_EQUAL_UINT32(img.size(), image);
  TEST_ASSERT_TRUE(sizeAsAnnounced(uint32_t(file.size()), image, trailer));  // Content-Length
  TEST_ASSERT_TRUE(sizeAsAnnounced(uint32_t(img.size()), image, trailer));   // the sidecar's image_size
  TEST_ASSERT_TRUE(sizeAsAnnounced(0, image, trailer));                      // not given
  TEST_ASSERT_FALSE(sizeAsAnnounced(uint32_t(file.size()) + 1, image, trailer));
  TEST_ASSERT_FALSE(sizeAsAnnounced(uint32_t(img.size()) - 1, image, trailer));
  // No trailer: the image is all of the file, the trailer's room is no excuse.
  TEST_ASSERT_TRUE(sizeAsAnnounced(5000, 5000, false));
  TEST_ASSERT_FALSE(sizeAsAnnounced(5128, 5000, false));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_head_ok_reads_version);
  RUN_TEST(test_head_refusals);
  RUN_TEST(test_head_long_strings_terminated);
  RUN_TEST(test_retry_pauses);
  RUN_TEST(test_pbkdf2_vectors);
  RUN_TEST(test_admin_password_check);
  RUN_TEST(test_url_forms);
  RUN_TEST(test_url_refusals);
  RUN_TEST(test_redirects);
  RUN_TEST(test_head_parse_split_anywhere);
  RUN_TEST(test_head_redirect_and_chunked);
  RUN_TEST(test_head_bad);
  RUN_TEST(test_chunked_any_split);
  RUN_TEST(test_chunked_bad);
  RUN_TEST(test_sha256_streaming);
  RUN_TEST(test_tool_signed_image_verifies);
  RUN_TEST(test_python_signature_and_tampering);
  RUN_TEST(test_upload_split_keeps_trailer_out);
  RUN_TEST(test_signed_file_size_is_as_announced);
  return UNITY_END();
}
