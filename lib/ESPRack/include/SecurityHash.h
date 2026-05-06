#ifndef SecurityHash_h
#define SecurityHash_h

#include <Arduino.h>
#include <esp_random.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/version.h>

namespace SecurityHash {

static constexpr size_t SALT_BYTES   = 16;   // 32 hex chars
static constexpr size_t HASH_BYTES   = 32;   // 64 hex chars (PBKDF2-SHA256 256-bit)
static constexpr size_t HASH_HEX_LEN = HASH_BYTES * 2;
static constexpr size_t SALT_HEX_LEN = SALT_BYTES * 2;
static constexpr int    ITERATIONS   = 2000;

inline String toHex(const uint8_t* buf, size_t len) {
  String out;
  out.reserve(len * 2);
  static const char* hexDigits = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    out += hexDigits[(buf[i] >> 4) & 0x0f];
    out += hexDigits[buf[i] & 0x0f];
  }
  return out;
}

inline bool fromHex(const String& hex, uint8_t* buf, size_t len) {
  if (hex.length() != len * 2) return false;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };
  for (size_t i = 0; i < len; ++i) {
    int hi = nib(hex[i * 2]);
    int lo = nib(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    buf[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

inline String makeSalt() {
  uint8_t saltBuf[SALT_BYTES];
  esp_fill_random(saltBuf, SALT_BYTES);
  return toHex(saltBuf, SALT_BYTES);
}

inline String hashPassword(const String& plaintext, const String& saltHex) {
  uint8_t saltBuf[SALT_BYTES];
  if (!fromHex(saltHex, saltBuf, SALT_BYTES)) return String();

  uint8_t out[HASH_BYTES];
  int rc;

#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x03000000)
  // mbedtls 3.x dropped the context-based PBKDF2 in favour of the
  // `_ext` variant that takes md_type directly.
  rc = mbedtls_pkcs5_pbkdf2_hmac_ext(
      MBEDTLS_MD_SHA256,
      reinterpret_cast<const unsigned char*>(plaintext.c_str()),
      plaintext.length(),
      saltBuf, SALT_BYTES,
      ITERATIONS,
      HASH_BYTES, out);
#else
  // mbedtls 2.x — context required.
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info || mbedtls_md_setup(&ctx, info, 1) != 0) {
    mbedtls_md_free(&ctx);
    return String();
  }
  rc = mbedtls_pkcs5_pbkdf2_hmac(
      &ctx,
      reinterpret_cast<const unsigned char*>(plaintext.c_str()),
      plaintext.length(),
      saltBuf, SALT_BYTES,
      ITERATIONS,
      HASH_BYTES, out);
  mbedtls_md_free(&ctx);
#endif

  if (rc != 0) return String();
  return toHex(out, HASH_BYTES);
}

// Constant-time compare so authenticate() does not leak which prefix
// matched via timing. Falls back to a non-match when lengths differ.
inline bool constantTimeEquals(const String& a, const String& b) {
  if (a.length() != b.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.length(); ++i) {
    diff |= (uint8_t)(a[i] ^ b[i]);
  }
  return diff == 0;
}

inline bool verify(const String& plaintext, const String& saltHex, const String& expectedHashHex) {
  if (saltHex.length() != SALT_HEX_LEN || expectedHashHex.length() != HASH_HEX_LEN) {
    return false;
  }
  String got = hashPassword(plaintext, saltHex);
  if (got.isEmpty()) return false;
  return constantTimeEquals(got, expectedHashHex);
}

// Quick check whether a stored "password" field already looks like a
// PBKDF2 hash (64 lowercase/uppercase hex chars). Used by the loader to
// distinguish legacy plaintext records that need on-load migration.
inline bool looksHashed(const String& s) {
  if (s.length() != HASH_HEX_LEN) return false;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!ok) return false;
  }
  return true;
}

}  // namespace SecurityHash

#endif  // end SecurityHash_h
