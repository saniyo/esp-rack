#include "system/OtaHandoff.h"

#include <stdio.h>

#include "persist/Crc32.h"

namespace ESPRack {
namespace ota {

namespace {

// The bytes between two fields (padding), zeroed so the CRC covers fixed values.
void zeroGap(void* rec, size_t from, size_t to) { memset(static_cast<uint8_t*>(rec) + from, 0, to - from); }

template <size_t N>
void terminate(char (&s)[N]) {
  s[N - 1] = 0;
}

template <typename T>
uint32_t crcOf(const T& r) {
  return crc32Ieee(&r, offsetof(T, crc));
}

void terminateAll(Handoff& h) {
  terminate(h.url);
  terminate(h.token);
  terminate(h.targetVersion);
  terminate(h.wifiSsid);
  terminate(h.wifiPass);
  terminate(h.adminHash);
}

void terminateAll(Result& r) {
  terminate(r.from);
  terminate(r.to);
  terminate(r.lastError);
}

void zeroPadding(Handoff& h) { zeroGap(&h, offsetof(Handoff, adminHash) + sizeof(h.adminHash), offsetof(Handoff, crc)); }

void zeroPadding(Result& r) {
  zeroGap(&r, offsetof(Result, to) + sizeof(r.to), offsetof(Result, attempts));
  zeroGap(&r, offsetof(Result, attempts) + sizeof(r.attempts), offsetof(Result, bytes));
}

// Magic and version first: a record of another version may have another size.
template <typename T>
Verdict check(const void* blob, size_t len, uint32_t magic, uint16_t version, T& out) {
  if (!blob || len < 6) return Verdict::BadSize;
  uint32_t m;
  uint16_t v;
  memcpy(&m, blob, sizeof(m));
  memcpy(&v, static_cast<const uint8_t*>(blob) + 4, sizeof(v));
  if (m != magic) return Verdict::BadMagic;
  if (v != version) return Verdict::OtherVersion;
  if (len != sizeof(T)) return Verdict::BadSize;
  T r;
  memcpy(&r, blob, sizeof(T));
  if (r.crc != crcOf(r)) return Verdict::BadCrc;
  terminateAll(r);
  out = r;
  return Verdict::Ok;
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

const char* modeName(uint8_t mode) {
  switch (static_cast<Mode>(mode)) {
    case Mode::None: return "none";
    case Mode::Push: return "push";
    case Mode::Pull: return "pull";
  }
  return "?";
}

const char* outcomeName(uint8_t outcome) {
  switch (static_cast<Outcome>(outcome)) {
    case Outcome::None: return "none";
    case Outcome::Ok: return "ok";
    case Outcome::FailedBeforeWrite: return "failed-before-write";
    case Outcome::FailedAfterWrite: return "failed-after-write";
    case Outcome::Aborted: return "aborted";
  }
  return "?";
}

const char* verdictName(Verdict v) {
  switch (v) {
    case Verdict::Ok: return "ok";
    case Verdict::BadSize: return "bad size";
    case Verdict::BadMagic: return "bad magic";
    case Verdict::OtherVersion: return "other version";
    case Verdict::BadCrc: return "bad crc";
  }
  return "?";
}

void encode(Handoff& h) {
  h.magic = kHandoffMagic;
  h.version = kHandoffVersion;
  if (h.sigLen > kSigMax) h.sigLen = 0;
  terminateAll(h);
  zeroPadding(h);
  h.crc = crcOf(h);
}

void encode(Result& r) {
  r.magic = kResultMagic;
  r.version = kResultVersion;
  terminateAll(r);
  zeroPadding(r);
  r.crc = crcOf(r);
}

Verdict decode(const void* blob, size_t len, Handoff& out) {
  Handoff h;
  const Verdict v = check(blob, len, kHandoffMagic, kHandoffVersion, h);
  if (v != Verdict::Ok) return v;
  if (h.sigLen > kSigMax) h.sigLen = 0;  // not ours (encode() never seals that): no signature
  out = h;
  return v;
}

Verdict decode(const void* blob, size_t len, Result& out) {
  return check(blob, len, kResultMagic, kResultVersion, out);
}

void clear(Handoff& h, Mode mode) {
  memset(&h, 0, sizeof(h));
  h.magic = kHandoffMagic;
  h.version = kHandoffVersion;
  h.mode = static_cast<uint8_t>(mode);
}

void clear(Result& r, Outcome outcome) {
  memset(&r, 0, sizeof(r));
  r.magic = kResultMagic;
  r.version = kResultVersion;
  r.outcome = static_cast<uint8_t>(outcome);
}

bool setText(char* dst, size_t cap, const char* src) {
  if (!dst || !cap) return false;
  if (!src) src = "";
  const size_t n = strlen(src);
  const size_t k = n < cap ? n : cap - 1;
  memcpy(dst, src, k);
  memset(dst + k, 0, cap - k);  // the rest zero: a blob of the same text is the same blob
  return k == n;
}

bool fromHex(const char* hex, uint8_t* out, size_t n) {
  if (!hex || strlen(hex) != 2 * n) return false;
  for (size_t i = 0; i < n; ++i) {
    const int hi = hexVal(hex[2 * i]);
    const int lo = hexVal(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>(hi << 4 | lo);
  }
  return true;
}

void toHex(const uint8_t* in, size_t n, char* out) {
  static const char kDigits[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    out[2 * i] = kDigits[in[i] >> 4];
    out[2 * i + 1] = kDigits[in[i] & 0x0F];
  }
  out[2 * n] = 0;
}

bool sigFromHex(const char* hex, uint8_t (&der)[kSigMax], uint8_t& len) {
  const size_t chars = hex ? strlen(hex) : 0;
  if (!chars || chars % 2 || chars / 2 > kSigMax) return false;
  if (!fromHex(hex, der, chars / 2)) return false;
  len = static_cast<uint8_t>(chars / 2);
  return true;
}

bool hasSha256(const Handoff& h) {
  for (uint8_t b : h.sha256)
    if (b) return true;
  return false;
}

size_t describe(const Result& r, char* out, size_t cap) {
  if (!out || !cap) return 0;
  const char* from = r.from[0] ? r.from : "?";
  const char* to = r.to[0] ? r.to : "?";
  const unsigned s = static_cast<unsigned>((r.durationMs + 500) / 1000);
  const unsigned a = r.attempts;
  int n;
  switch (static_cast<Outcome>(r.outcome)) {
    case Outcome::Ok:
      n = snprintf(out, cap, "ok: %s -> %s, %u attempt%s, %lu B in %u s", from, to, a, a == 1 ? "" : "s",
                   static_cast<unsigned long>(r.bytes), s);
      break;
    case Outcome::FailedBeforeWrite:
    case Outcome::Aborted:
      n = snprintf(out, cap, "%s: %s (%s kept, %u attempt%s)", outcomeName(r.outcome),
                   r.lastError[0] ? r.lastError : "no reason given", from, a, a == 1 ? "" : "s");
      break;
    case Outcome::FailedAfterWrite:
      n = snprintf(out, cap, "%s: %s (%u attempt%s, %lu B)", outcomeName(r.outcome),
                   r.lastError[0] ? r.lastError : "no reason given", a, a == 1 ? "" : "s",
                   static_cast<unsigned long>(r.bytes));
      break;
    default:
      n = snprintf(out, cap, "%s", outcomeName(r.outcome));
      break;
  }
  if (n < 0) {
    out[0] = 0;
    return 0;
  }
  return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

TrailerVerdict parseTrailer(const uint8_t* tail, uint32_t fileLen, SigTrailer& out) {
  if (!tail || fileLen < kTrailerSize) return TrailerVerdict::None;
  if (memcmp(tail, kTrailerMagic, 8) != 0) return TrailerVerdict::None;
  uint32_t len, crc;
  memcpy(&len, tail + 8, 4);  // little-endian on every target and host here
  memcpy(&crc, tail + 124, 4);
  const uint8_t sigLen = tail[12];
  if (crc != crc32Ieee(tail, 124)) return TrailerVerdict::Corrupt;
  if (len != fileLen - kTrailerSize || sigLen == 0 || sigLen > kSigMax) return TrailerVerdict::Corrupt;
  out.imageLen = len;
  out.sigLen = sigLen;
  memset(out.der, 0, sizeof(out.der));
  memcpy(out.der, tail + 13, sigLen);
  return TrailerVerdict::Ok;
}

bool encodeTrailer(uint32_t imageLen, const uint8_t* der, uint8_t derLen, uint8_t* out) {
  if (!out || !der || derLen == 0 || derLen > kSigMax) return false;
  memset(out, 0, kTrailerSize);
  memcpy(out, kTrailerMagic, 8);
  memcpy(out + 8, &imageLen, 4);
  out[12] = derLen;
  memcpy(out + 13, der, derLen);
  const uint32_t crc = crc32Ieee(out, 124);
  memcpy(out + 124, &crc, 4);
  return true;
}

}  // namespace ota
}  // namespace ESPRack
