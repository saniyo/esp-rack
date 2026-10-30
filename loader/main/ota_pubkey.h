// PLACEHOLDER (loader/ as committed): a dev key whose private half was thrown
// away, so nothing verifies against it and every image goes in with the "dev
// signing key" warning. `tools/ota_sign.py export-pubkey` replaces this file
// with the key in use (the dev key it makes on first use, or a production one).
// The OTA loader checks image signatures (ECDSA P-256 over SHA-256) with it.
// DEV KEY: no production key was configured. The loader logs "dev signing
// key" and installs unsigned or badly signed images anyway. `ota_sign.py
// keygen`, then export again, makes signatures mandatory.
#pragma once

#include <stdint.h>

#define ESPRACK_OTA_KEY_DEV 1  // 1: a dev key (warn only), 0: signatures mandatory
#define ESPRACK_OTA_KEY_ID "e1c2a8c701206ff4"  // SHA-256 of the point, first 8 bytes

// The public point, uncompressed SEC1: 0x04 || X || Y.
static const uint8_t esprack_ota_pubkey[65] = {
  0x04, 0xb6, 0x57, 0x50, 0x9d, 0x9c, 0xb0, 0x9a, 0x9c, 0x65, 0xac, 0xcd, 0xca,
  0x98, 0x9e, 0x4c, 0xab, 0xa7, 0xb2, 0x65, 0x78, 0xb0, 0x66, 0x21, 0xe9, 0x74,
  0xb8, 0x23, 0xf1, 0x19, 0x70, 0xe3, 0xfb, 0x34, 0x08, 0xfd, 0xfe, 0xaa, 0x42,
  0x23, 0xe6, 0xbf, 0xeb, 0x4f, 0xdf, 0x43, 0x4c, 0x2c, 0xe2, 0x7f, 0x62, 0xde,
  0xda, 0xf3, 0xa9, 0x4d, 0x14, 0x6d, 0x83, 0x09, 0xea, 0x54, 0xdf, 0x39, 0x1f
};

// The same key as PEM (SubjectPublicKeyInfo); for mbedtls_pk_parse_public_key
// pass sizeof (it counts the terminating NUL).
static const char esprack_ota_pubkey_pem[] =
  "-----BEGIN PUBLIC KEY-----\n"
  "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEtldQnZywmpxlrM3KmJ5Mq6eyZXiw\n"
  "ZiHpdLgj8Rlw4/s0CP3+qkIj5r/rT99DTCzif2Le2vOpTRRtgwnqVN85Hw==\n"
  "-----END PUBLIC KEY-----\n";
