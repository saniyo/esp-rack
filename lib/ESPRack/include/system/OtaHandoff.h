#pragma once
#ifndef ESPRACK_SYSTEM_OTA_HANDOFF_H
#define ESPRACK_SYSTEM_OTA_HANDOFF_H

// OtaHandoff: what the app and the update loader (the `loader` partition
// scheme, system/OtaLoader.h; the loader itself is the ESP-IDF project in
// loader/) pass each other through NVS, and the trailer of a signed image.
//
//   app    -> loader   NVS "esprack_ota"/"handoff"   what to install, how (Handoff)
//   loader -> app      NVS "esprack_ota"/"result"    how it went (Result)
//
// Each blob is its struct as it lies in memory (one chip, one compiler
// family: little-endian, natural alignment; the offsets are pinned below),
// with a CRC-32 (persist/Crc32.h) over every byte before `crc`, padding
// zeroed. encode() seals a record in place (magic, version, terminated
// strings, zeroed padding, CRC): it is then the blob. decode() checks
// magic, version, size and CRC, and terminates every string of the copy.
// A new layout is a new version: the other side refuses the record
// (OtherVersion) rather than misread it, so an app and a loader of
// different record versions fall back to "no handoff" / "no result".
// The one exception: decode() still takes a version 1 handoff (an app
// from before version 2) and gives it as a version 2 one (HandoffV1).
//
// The contract around the records:
//   - the app writes the handoff, sets the boot partition to factory and
//     restarts (OtaLoader::enter); nothing else writes it;
//   - the loader writes a Result before EVERY restart into the app (the
//     app tells a boot back from the loader by it), and erases the handoff
//     once it is done with it (it holds a Wi-Fi password and a token);
//   - the app reads the Result once at boot and erases it.
//
// A signed image is one file: the image bytes, then a 128-byte trailer
//   off   0  magic "ESRKSIG1"             8 bytes
//         8  imageLen   u32 LE            the bytes before the trailer
//        12  sigLen     u8                DER length, 8..72
//        13  der[72]                      ECDSA P-256 over SHA-256(image bytes), DER
//        85  pad                          39 zero bytes
//       124  crc        u32 LE            CRC-32 (IEEE, zlib's) of bytes 0..123
// so a file whose last 128 bytes parse as a trailer for its length is a
// signed image (parseTrailer); otherwise all of it is the image. A pull
// handoff carries the SHA-256 and the signature itself (all zero / sigLen
// 0: not given; the loader then looks for a trailer in the download).
//
// Arduino-free and IDF-free: the app and the loader both build it
// (src/system/OtaHandoff.cpp + src/persist/Crc32.cpp). Host-tested.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ESPRack {
namespace ota {

constexpr const char* kNvsNamespace = "esprack_ota";
constexpr const char* kNvsHandoffKey = "handoff";
constexpr const char* kNvsResultKey = "result";

constexpr uint32_t kHandoffMagic = 0x3148544Fu;  // bytes 'O','T','H','1'
constexpr uint32_t kResultMagic = 0x3152544Fu;   // bytes 'O','T','R','1'
constexpr uint16_t kHandoffVersion = 2;
constexpr uint16_t kHandoffVersionV1 = 1;  // still decoded (HandoffV1)
constexpr uint16_t kResultVersion = 1;

// Handoff::mode
enum class Mode : uint8_t {
  None = 0,
  Push = 1,  // the loader serves its upload page and waits for an image
  Pull = 2   // the loader downloads the image from url
};
// Result::outcome
enum class Outcome : uint8_t {
  None = 0,
  Ok = 1,                 // the new image is in and boots
  FailedBeforeWrite = 2,  // nothing written: the old app boots again
  FailedAfterWrite = 3,   // the app slot was written in part: the loader keeps trying
  Aborted = 4             // given up without an image (e.g. push mode, nobody uploaded): the old app boots again
};
// Handoff::adminKdf: how the upload page's password is checked.
enum class AdminKdf : uint8_t {
  None = 0,          // no password: the page is open (it warns)
  Sha256 = 1,        // adminKey = SHA-256(password): a version 1 handoff
  Pbkdf2Sha256 = 2   // adminKey = PBKDF2-HMAC-SHA256(password, adminSalt, adminIter, 32 B):
                     // the app's stored user record (SecurityHash.h), so the loader asks
                     // for that admin's own password
};
const char* modeName(uint8_t mode);        // "none" "push" "pull" "?"
const char* outcomeName(uint8_t outcome);  // "none" "ok" "failed-before-write" "failed-after-write" "aborted" "?"

constexpr size_t kSha256Len = 32;
constexpr size_t kSigMax = 72;  // an ECDSA P-256 signature in DER: 70-72 bytes, rarely less
constexpr size_t kAdminSaltMax = 32;
constexpr size_t kAdminKeyLen = 32;
constexpr uint32_t kAdminIterMax = 100000;  // PBKDF2 rounds a loader runs per password check, at most
constexpr size_t kApPassMin = 8;   // WPA2-PSK passphrase: 8..63 printable ASCII
constexpr size_t kApPassMax = 63;

// Version 2. The first 544 bytes are version 1's (HandoffV1); its
// adminHash went, the admin record and the rest came.
struct Handoff {
  uint32_t magic;
  uint16_t version;
  uint8_t mode;             // Mode
  uint8_t attemptsAllowed;  // downloads after the first flash write before the loader calls the update stuck (it goes on); 0: its default
  char url[200];            // pull: the image (http://...)
  char token[96];           // pull: a download token for the server, sent on every retry (valid up to the install), "" none
  uint32_t size;            // pull: the image's bytes or the signed file's (image + trailer), 0 unknown
  uint8_t sha256[32];       // pull: SHA-256 of the image bytes, all zero: not given
  uint8_t sig[72];          // pull: ECDSA P-256 signature over sha256 (DER)
  uint8_t sigLen;           //   its length, 0: none
  char targetVersion[32];   // the version expected to come, "" unknown (shown, and in the Result)
  char wifiSsid[33];        // the station the app was on ("" with useShared)
  char wifiPass[65];
  uint8_t useShared;        // 1: the Wi-Fi settings the loader keeps itself (IDF's stored station config)
  // The upload page's password check (AdminKdf; None: the page is open and warns).
  uint8_t adminKdf;
  uint8_t adminSaltLen;     // 1..kAdminSaltMax with Pbkdf2Sha256
  uint32_t adminIter;       // PBKDF2 rounds, 1..kAdminIterMax
  uint8_t adminSalt[32];
  uint8_t adminKey[32];     // what the password must derive to
  char adminUser[32];       // whose password it is (shown on the page), "" not known
  char apPass[64];          // the loader's fallback AP: WPA2-PSK with this (8..63 chars), "" open (it warns)
  char deviceId[64];        // the device's id for the image server (x-ESPRack-Device-Id), "" none
  uint32_t crc;
};
static_assert(offsetof(Handoff, url) == 8, "handoff layout");
static_assert(offsetof(Handoff, size) == 304, "handoff layout");
static_assert(offsetof(Handoff, sigLen) == 412, "handoff layout");
static_assert(offsetof(Handoff, adminKdf) == 544 && offsetof(Handoff, adminIter) == 548, "handoff layout");
static_assert(offsetof(Handoff, adminUser) == 616 && offsetof(Handoff, apPass) == 648, "handoff layout");
static_assert(offsetof(Handoff, crc) == 776 && sizeof(Handoff) == 780, "handoff layout");

// Version 1 (an app from before version 2 wrote it): the same first 544
// bytes, then the hex SHA-256 of the upload page's password. decode()
// gives it as a version 2 Handoff (adminKdf Sha256, no AP password, no
// device id).
struct HandoffV1 {
  uint32_t magic;
  uint16_t version;
  uint8_t mode;
  uint8_t attemptsAllowed;
  char url[200];
  char token[96];
  uint32_t size;
  uint8_t sha256[32];
  uint8_t sig[72];
  uint8_t sigLen;
  char targetVersion[32];
  char wifiSsid[33];
  char wifiPass[65];
  uint8_t useShared;
  char adminHash[65];       // hex SHA-256 of the upload page's password, "" none
  uint32_t crc;
};
static_assert(offsetof(HandoffV1, adminHash) == offsetof(Handoff, adminKdf), "handoff v1 layout");
static_assert(offsetof(HandoffV1, crc) == 612 && sizeof(HandoffV1) == 616, "handoff v1 layout");

struct Result {
  uint32_t magic;
  uint16_t version;
  uint8_t outcome;      // Outcome
  char from[32];        // the app version before ("" unknown)
  char to[32];          // the version installed (ok) or wanted
  uint16_t attempts;    // downloads / uploads tried
  uint32_t bytes;       // written into the app slot
  uint32_t durationMs;  // in the loader, all attempts
  char lastError[64];   // the last failure, "" none
  uint32_t crc;
};
static_assert(offsetof(Result, from) == 7 && offsetof(Result, attempts) == 72, "result layout");
static_assert(offsetof(Result, bytes) == 76 && offsetof(Result, lastError) == 84, "result layout");
static_assert(offsetof(Result, crc) == 148 && sizeof(Result) == 152, "result layout");

enum class Verdict : uint8_t {
  Ok,
  BadSize,       // not this record's size (a truncated blob)
  BadMagic,      // not this record at all
  OtherVersion,  // this record, another layout
  BadCrc
};
const char* verdictName(Verdict v);

// Seal in place: magic, version, every string terminated, padding zeroed,
// CRC. The struct is then the blob (sizeof bytes). Fields out of range
// are cleared first (as decode() does): a sigLen past kSigMax, an admin
// record that can't be checked, an AP password that WPA2 doesn't take.
void encode(Handoff& h);
void encode(Result& r);
// A stored blob of len bytes into out (only on Ok; strings terminated,
// fields out of range cleared as by encode()). A handoff may be version
// 1 or 2 (HandoffV1); out is always version 2.
Verdict decode(const void* blob, size_t len, Handoff& out);
Verdict decode(const void* blob, size_t len, Result& out);

// A zeroed record (padding too) of the given mode / outcome, magic and
// version set; fill it, then encode().
void clear(Handoff& h, Mode mode = Mode::None);
void clear(Result& r, Outcome outcome = Outcome::None);

// Copies src into a fixed field, always terminated; false: cut short.
bool setText(char* dst, size_t cap, const char* src);
template <size_t N>
inline bool setText(char (&dst)[N], const char* src) { return setText(dst, N, src); }

// Hex (either case) to exactly n bytes; false: not 2n hex digits.
bool fromHex(const char* hex, uint8_t* out, size_t n);
// n bytes as 2n lower-case hex digits and a NUL (out: 2n + 1).
void toHex(const uint8_t* in, size_t n, char* out);
// The DER signature from hex (up to kSigMax bytes); false: not hex, odd,
// empty or too long.
bool sigFromHex(const char* hex, uint8_t (&der)[kSigMax], uint8_t& len);
// sha256 all zero: not given.
bool hasSha256(const Handoff& h);
// The upload page has a password to check (adminKdf not None).
bool hasAdmin(const Handoff& h);
// A WPA2-PSK passphrase: 8..63 printable ASCII characters.
bool apPassOk(const char* pass);

// One line for a page or a log: "ok: 1.2 -> 1.3, 1 attempt, 1250000 B in 42 s"
// ("failed-before-write: <error> (the old app kept)", ...). Returns the
// length written (cut to cap - 1).
size_t describe(const Result& r, char* out, size_t cap);

// ---- the signature trailer of a signed image ----

constexpr size_t kTrailerSize = 128;
constexpr char kTrailerMagic[9] = "ESRKSIG1";  // 8 bytes on the wire, no NUL

struct SigTrailer {
  uint32_t imageLen;
  uint8_t sigLen;
  uint8_t der[kSigMax];
};

enum class TrailerVerdict : uint8_t {
  Ok,
  None,    // no trailer magic: the whole file is the image (unsigned)
  Corrupt  // the magic, but a bad CRC, length or sigLen: refuse the file
};
// tail: the file's last kTrailerSize bytes; fileLen: the whole file's
// length (imageLen must be fileLen - kTrailerSize).
TrailerVerdict parseTrailer(const uint8_t* tail, uint32_t fileLen, SigTrailer& out);
// The trailer for an image of imageLen bytes (out: kTrailerSize bytes).
// false: derLen 0 or > kSigMax.
bool encodeTrailer(uint32_t imageLen, const uint8_t* der, uint8_t derLen, uint8_t* out);

// A file streamed through with its last kTrailerSize bytes held back,
// since a trailer must reach neither the flash nor the hash: feed() hands
// on what is surely image (sink(const uint8_t*, size_t) -> bool, at most
// two pieces a call, in order); at the end tail() holds the last held()
// bytes: a trailer (parseTrailer), or the image's own end, then handed on
// by the caller too.
class TailHold {
 public:
  template <typename Sink>
  bool feed(const uint8_t* p, size_t n, Sink&& sink) {
    if (_n + n <= kTrailerSize) {
      memcpy(_buf + _n, p, n);
      _n += n;
      return true;
    }
    const size_t out = _n + n - kTrailerSize;  // bytes that can no longer be trailer
    const size_t fromHeld = out < _n ? out : _n;
    if (fromHeld && !sink(static_cast<const uint8_t*>(_buf), fromHeld)) return false;
    const size_t fromIn = out - fromHeld;
    if (fromIn && !sink(p, fromIn)) return false;
    memmove(_buf, _buf + fromHeld, _n - fromHeld);
    memcpy(_buf + (_n - fromHeld), p + fromIn, n - fromIn);
    _n = kTrailerSize;
    return true;
  }
  size_t held() const { return _n; }
  const uint8_t* tail() const { return _buf; }
  void reset() { _n = 0; }

 private:
  uint8_t _buf[kTrailerSize] = {};
  size_t _n = 0;
};

}  // namespace ota
}  // namespace ESPRack

#endif  // ESPRACK_SYSTEM_OTA_HANDOFF_H
