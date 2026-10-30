#pragma once
#ifndef ESPRACK_LOADER_SIG_VERIFY_H
#define ESPRACK_LOADER_SIG_VERIFY_H

// SHA-256 and the ECDSA P-256 check of an image's signature, and the
// upload page's password check, on mbedtls only (the target's, or a host
// build of it: loader/test-host).
//
// The signature is ECDSA-with-SHA-256 over the whole image, DER-encoded
// (SEQUENCE { INTEGER r, INTEGER s }, 72 B at most): what Python's
// cryptography or `openssl dgst -sha256 -sign` produce from the image.

#include <stddef.h>
#include <stdint.h>

#include "system/OtaHandoff.h"

namespace loader {

// One SHA-256 under way (the image as it streams). Opaque storage so this
// header needs no mbedtls one.
class Sha256 {
 public:
  Sha256();
  ~Sha256();
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;
  void restart();
  void update(const void* p, size_t n);
  void finish(uint8_t out[32]);  // restart() before reuse

 private:
  alignas(8) uint8_t _ctx[256];
};

void sha256(const void* p, size_t n, uint8_t out[32]);

enum class Sig : uint8_t {
  Ok,
  Bad,     // a well-formed signature that doesn't match
  Format,  // not DER r,s / wrong length
  NoKey    // the public key isn't a P-256 point
};
const char* sigText(Sig s);

// pub: SEC1 uncompressed point (0x04 | X | Y, 65 B). hash: the image's
// SHA-256.
Sig verifyP256(const uint8_t pub[65], const uint8_t hash[32], const uint8_t* der, size_t derLen);

// PBKDF2-HMAC-SHA256 (mbedtls pkcs5, as the app's SecurityHash.h), keyLen
// bytes into out. false: mbedtls refused (a bad argument).
bool pbkdf2Sha256(const void* pw, size_t n, const uint8_t* salt, size_t saltLen, uint32_t rounds, uint8_t* out,
                  size_t keyLen);
// The password (n bytes, UTF-8) against the handoff's admin record:
// SHA-256 (a version 1 handoff) or PBKDF2 over its salt and rounds, the
// result compared in constant time. false: no record, or no match.
bool adminPasswordOk(const ESPRack::ota::Handoff& h, const char* pw, size_t n);

}  // namespace loader

#endif  // ESPRACK_LOADER_SIG_VERIFY_H
