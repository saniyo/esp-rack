#ifndef SecretsVault_h
#define SecretsVault_h

// SecretsVault — AES-128-CBC encryption for sensitive config fields,
// keyed off the chip's eFuse MAC so each device has a unique key that
// never leaves RAM.
//
// Format on disk: "ENC:" + lowercase-hex(iv:16 || ciphertext:N) where
// N is a multiple of 16 (PKCS7-padded). decrypt() is idempotent:
// strings without the "ENC:" prefix pass through unchanged so that
// HTTP POST input (always plaintext from the React form) and legacy
// plaintext config files both round-trip correctly.
//
// Failure modes the caller should be prepared for:
//   * keyAvailable() == false  →  encrypt is no-op, decrypt is no-op.
//                                 Both directions degrade to plaintext.
//                                 One-time warning printed at first call.
//   * decrypt(garbage)         →  empty string. Service sees an empty
//                                 password and behaves as "no password
//                                 configured" — operator retypes via UI.
//   * device swap              →  eFuse differs → key differs → every
//                                 ENC: blob decrypts to "" → operator
//                                 re-enters credentials once.
//   * firmware reflash         →  same chip, same MAC, same key — no
//                                 user-visible effect.
//
// Compile-time toggle (features.ini → -D FT_SECRETS_VAULT=…):
//   FT_SECRETS_VAULT == 1  →  full implementation, mbedtls AES + SHA-256
//                             + eFuse MAC linked, ENC: blobs on disk.
//   FT_SECRETS_VAULT == 0  →  EVERYTHING below is preprocessed out.
//                             encrypt/decrypt become pass-through inline
//                             stubs, no mbedtls / esp_mac symbols are
//                             pulled in, ConfigManager skips its
//                             secret-key walk entirely. Existing ENC:
//                             records on disk become unreadable —
//                             operator must re-enter credentials.

#include <Arduino.h>
#include <Features.h>

#if FT_ENABLED(FT_SECRETS_VAULT)
#include <esp_mac.h>
#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include <string.h>
#endif

namespace SecretsVault {

#if FT_ENABLED(FT_SECRETS_VAULT)

static constexpr size_t kSvIvBytes  = 16;
static constexpr size_t kSvKeyBytes = 16;  // AES-128
static constexpr const char* kSvPrefix = "ENC:";
static constexpr size_t kSvPrefixLen  = 4;
static constexpr const char* kSvKdfDomain = "ESP-SES-secrets-v1";

namespace detail {

inline String hexEncode(const uint8_t* buf, size_t len) {
  String out;
  out.reserve(len * 2);
  static const char* digits = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    out += digits[(buf[i] >> 4) & 0x0f];
    out += digits[buf[i] & 0x0f];
  }
  return out;
}

inline bool hexDecode(const char* hex, size_t hexLen, uint8_t* out, size_t outLen) {
  if (hexLen != outLen * 2) return false;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };
  for (size_t i = 0; i < outLen; ++i) {
    int hi = nib(hex[i * 2]);
    int lo = nib(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

// Derives the AES-128 key from the chip's eFuse MAC the first time
// it's called. Returns nullptr if the eFuse read fails — caller must
// handle gracefully (see keyAvailable()).
inline const uint8_t* derivedKey() {
  static uint8_t key[kSvKeyBytes];
  static bool ready = false;
  static bool tried = false;
  if (ready) return key;
  if (tried) return nullptr;
  tried = true;

  uint8_t mac[6] = {0};
  if (esp_efuse_mac_get_default(mac) != ESP_OK) {
    log_e("[secrets] eFuse MAC read failed; encryption disabled");
    return nullptr;
  }

  // SHA-256(mac || domain) → take first 16 bytes
  uint8_t digest[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x03000000)
  if (mbedtls_sha256_starts(&ctx, 0) != 0) {
    mbedtls_sha256_free(&ctx);
    return nullptr;
  }
  mbedtls_sha256_update(&ctx, mac, sizeof(mac));
  mbedtls_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(kSvKdfDomain), strlen(kSvKdfDomain));
  mbedtls_sha256_finish(&ctx, digest);
#else
  if (mbedtls_sha256_starts_ret(&ctx, 0) != 0) {
    mbedtls_sha256_free(&ctx);
    return nullptr;
  }
  mbedtls_sha256_update_ret(&ctx, mac, sizeof(mac));
  mbedtls_sha256_update_ret(&ctx, reinterpret_cast<const uint8_t*>(kSvKdfDomain), strlen(kSvKdfDomain));
  mbedtls_sha256_finish_ret(&ctx, digest);
#endif
  mbedtls_sha256_free(&ctx);

  memcpy(key, digest, kSvKeyBytes);
  ready = true;
  return key;
}

inline void warnOnce(const __FlashStringHelper* msg) {
  static bool warned = false;
  if (warned) return;
  warned = true;
  // __FlashStringHelper* points at a PROGMEM string literal — on ESP32
  // flash is memory-mapped so reinterpret_cast to const char* is safe.
  log_w("%s", reinterpret_cast<const char*>(msg));
}

}  // namespace detail

inline bool keyAvailable() {
  return detail::derivedKey() != nullptr;
}

inline bool isEncrypted(const String& s) {
  if (s.length() < kSvPrefixLen + (kSvIvBytes * 2) + 32) return false;  // need at least IV + 1 block
  return strncmp(s.c_str(), kSvPrefix, kSvPrefixLen) == 0;
}

inline String encrypt(const String& plain) {
#ifdef SECRETS_VAULT_DISABLED
  return plain;
#else
  const uint8_t* key = detail::derivedKey();
  if (!key) {
    detail::warnOnce(F("[secrets] key unavailable; storing plaintext"));
    return plain;
  }
  if (plain.isEmpty()) {
    // Empty value stays empty — no point encrypting nothing, and it's
    // a useful signal that the field is unset.
    return plain;
  }

  // PKCS7 pad to 16-byte boundary.
  size_t in_len = plain.length();
  size_t pad = kSvIvBytes - (in_len % kSvIvBytes);
  size_t out_len = in_len + pad;

  // Stack-allocate up to 192 bytes; longer secrets fall back to heap.
  // Tokens / passwords are typically < 128 chars, so the stack path
  // is the common case.
  uint8_t stackBuf[192];
  uint8_t* buf = (out_len <= sizeof(stackBuf)) ? stackBuf : (uint8_t*)malloc(out_len);
  if (!buf) return plain;
  memcpy(buf, plain.c_str(), in_len);
  for (size_t i = in_len; i < out_len; ++i) buf[i] = (uint8_t)pad;

  uint8_t iv[kSvIvBytes];
  esp_fill_random(iv, kSvIvBytes);

  // CBC modifies its IV argument in-place — keep a copy for hex output.
  uint8_t ivCopy[kSvIvBytes];
  memcpy(ivCopy, iv, kSvIvBytes);

  uint8_t* cipher = (uint8_t*)malloc(out_len);
  if (!cipher) {
    if (buf != stackBuf) free(buf);
    return plain;
  }

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  int rc = mbedtls_aes_setkey_enc(&aes, key, kSvKeyBytes * 8);
  if (rc == 0) {
    rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, out_len, iv, buf, cipher);
  }
  mbedtls_aes_free(&aes);

  if (buf != stackBuf) free(buf);

  if (rc != 0) {
    free(cipher);
    return plain;
  }

  String out;
  out.reserve(kSvPrefixLen + (kSvIvBytes + out_len) * 2);
  out += kSvPrefix;
  out += detail::hexEncode(ivCopy, kSvIvBytes);
  out += detail::hexEncode(cipher, out_len);
  free(cipher);
  return out;
#endif
}

inline String decrypt(const String& maybe) {
  if (!isEncrypted(maybe)) return maybe;

  const uint8_t* key = detail::derivedKey();
  if (!key) {
    detail::warnOnce(F("[secrets] key unavailable; cannot decrypt"));
    return String();
  }

  size_t hexBytes = maybe.length() - kSvPrefixLen;
  if (hexBytes < (kSvIvBytes * 2) || (hexBytes % 2) != 0) return String();
  size_t cipherHex = hexBytes - (kSvIvBytes * 2);
  if (cipherHex == 0 || (cipherHex % 32) != 0) return String();
  size_t cipherLen = cipherHex / 2;

  uint8_t iv[kSvIvBytes];
  if (!detail::hexDecode(maybe.c_str() + kSvPrefixLen, kSvIvBytes * 2, iv, kSvIvBytes)) {
    return String();
  }

  uint8_t* cipher = (uint8_t*)malloc(cipherLen);
  if (!cipher) return String();
  if (!detail::hexDecode(maybe.c_str() + kSvPrefixLen + (kSvIvBytes * 2), cipherHex, cipher, cipherLen)) {
    free(cipher);
    return String();
  }

  uint8_t* out = (uint8_t*)malloc(cipherLen);
  if (!out) {
    free(cipher);
    return String();
  }

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  int rc = mbedtls_aes_setkey_dec(&aes, key, kSvKeyBytes * 8);
  if (rc == 0) {
    rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, cipherLen, iv, cipher, out);
  }
  mbedtls_aes_free(&aes);
  free(cipher);

  if (rc != 0) {
    free(out);
    return String();
  }

  // PKCS7 strip — last byte tells how many trailing bytes are padding.
  uint8_t pad = out[cipherLen - 1];
  if (pad == 0 || pad > kSvIvBytes || pad > cipherLen) {
    free(out);
    return String();
  }
  // Verify all padding bytes match (defends against random bytes that
  // happen to look like valid padding length).
  for (size_t i = cipherLen - pad; i < cipherLen; ++i) {
    if (out[i] != pad) {
      free(out);
      return String();
    }
  }

  size_t plainLen = cipherLen - pad;
  String result;
  result.reserve(plainLen);
  for (size_t i = 0; i < plainLen; ++i) result += (char)out[i];
  free(out);
  return result;
}

#else  // !FT_ENABLED(FT_SECRETS_VAULT)

// ---- Disabled-mode stubs ------------------------------------------------
// No mbedtls / esp_mac symbols pulled in; encrypt and decrypt are pure
// pass-through. Existing ENC:<hex> records on disk pass through to the
// caller as-is — services treat the literal "ENC:..." string as the
// password, which is wrong on purpose: it forces the operator to notice
// the broken credential and re-enter it after disabling encryption.
inline bool   keyAvailable()                  { return false; }
inline bool   isEncrypted(const String&)      { return false; }
inline String encrypt(const String& plain)    { return plain; }
inline String decrypt(const String& maybe)    { return maybe; }

#endif  // FT_ENABLED(FT_SECRETS_VAULT)

}  // namespace SecretsVault

#endif  // end SecretsVault_h
