#include "sig_verify.h"

#include <mbedtls/asn1.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/sha256.h>
#include <string.h>

namespace loader {

static_assert(sizeof(mbedtls_sha256_context) <= 256 && alignof(mbedtls_sha256_context) <= 8,
              "Sha256::_ctx too small");

namespace {
mbedtls_sha256_context* ctx(uint8_t* p) { return reinterpret_cast<mbedtls_sha256_context*>(p); }
}  // namespace

Sha256::Sha256() {
  mbedtls_sha256_init(ctx(_ctx));
  (void)mbedtls_sha256_starts(ctx(_ctx), 0);
}

Sha256::~Sha256() { mbedtls_sha256_free(ctx(_ctx)); }

void Sha256::restart() {
  mbedtls_sha256_free(ctx(_ctx));
  mbedtls_sha256_init(ctx(_ctx));
  (void)mbedtls_sha256_starts(ctx(_ctx), 0);
}

void Sha256::update(const void* p, size_t n) {
  (void)mbedtls_sha256_update(ctx(_ctx), static_cast<const unsigned char*>(p), n);
}

void Sha256::finish(uint8_t out[32]) { (void)mbedtls_sha256_finish(ctx(_ctx), out); }

void sha256(const void* p, size_t n, uint8_t out[32]) {
  (void)mbedtls_sha256(static_cast<const unsigned char*>(p), n, out, 0);
}

const char* sigText(Sig s) {
  switch (s) {
    case Sig::Ok:
      return "signature good";
    case Sig::Bad:
      return "signature does not match";
    case Sig::Format:
      return "signature malformed";
    case Sig::NoKey:
      return "no valid public key";
  }
  return "?";
}

Sig verifyP256(const uint8_t pub[65], const uint8_t hash[32], const uint8_t* der, size_t derLen) {
  mbedtls_ecp_group grp;
  mbedtls_ecp_point q;
  mbedtls_mpi r, s;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&q);
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);
  Sig out = Sig::Ok;
  unsigned char* p = const_cast<unsigned char*>(der);
  const unsigned char* end = der + derLen;
  size_t len = 0;
  if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0 ||
      mbedtls_ecp_point_read_binary(&grp, &q, pub, 65) != 0 || mbedtls_ecp_check_pubkey(&grp, &q) != 0) {
    out = Sig::NoKey;
  } else if (!der || mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0 ||
             p + len != end || mbedtls_asn1_get_mpi(&p, end, &r) != 0 || mbedtls_asn1_get_mpi(&p, end, &s) != 0 ||
             p != end) {
    out = Sig::Format;
  } else if (mbedtls_ecdsa_verify(&grp, hash, 32, &q, &r, &s) != 0) {
    out = Sig::Bad;
  }
  mbedtls_mpi_free(&s);
  mbedtls_mpi_free(&r);
  mbedtls_ecp_point_free(&q);
  mbedtls_ecp_group_free(&grp);
  return out;
}

bool pbkdf2Sha256(const void* pw, size_t n, const uint8_t* salt, size_t saltLen, uint32_t rounds, uint8_t* out,
                  size_t keyLen) {
  return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, static_cast<const unsigned char*>(pw), n, salt, saltLen,
                                       rounds, uint32_t(keyLen), out) == 0;
}

bool adminPasswordOk(const ESPRack::ota::Handoff& h, const char* pw, size_t n) {
  namespace ota = ESPRack::ota;
  uint8_t key[ota::kAdminKeyLen];
  switch (static_cast<ota::AdminKdf>(h.adminKdf)) {
    case ota::AdminKdf::Sha256:
      sha256(pw, n, key);
      break;
    case ota::AdminKdf::Pbkdf2Sha256:
      if (!h.adminSaltLen || h.adminSaltLen > ota::kAdminSaltMax || !h.adminIter || h.adminIter > ota::kAdminIterMax ||
          !pbkdf2Sha256(pw, n, h.adminSalt, h.adminSaltLen, h.adminIter, key, sizeof(key)))
        return false;
      break;
    default:
      return false;
  }
  uint8_t diff = 0;
  for (size_t i = 0; i < sizeof(key); ++i) diff |= uint8_t(key[i] ^ h.adminKey[i]);
  memset(key, 0, sizeof(key));
  return diff == 0;
}

}  // namespace loader
