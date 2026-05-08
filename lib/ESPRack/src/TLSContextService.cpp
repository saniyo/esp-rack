#include "TLSContextService.h"

#include <WiFiClientSecure.h>

// Phase 1.0 SKELETON implementation. All methods compile, store /
// return what they're given, but do not actually wire mbedtls
// contexts yet. This lets:
//   - App hold the singleton via setTls() / tls()
//   - CertManagerService call updateClientCert() when it has new
//     material (Phase 1.2 onwards)
//   - Consumer modules call attachToClient() without crashing,
//     getting a plain WiFiClientSecure they can still use in
//     setInsecure() / manual-cert mode for now
//
// Phase 1.1 will add real PEM-to-mbedtls parsing here, plus the
// WiFiClientSecure::setCACert / setCertificate / setPrivateKey
// calls that bind the chain to the outbound socket.

TLSContextService::TLSContextService() = default;
TLSContextService::~TLSContextService() = default;

bool TLSContextService::loadCaChain(const String& caBundlePem) {
  // Phase 1.1: parse with mbedtls_x509_crt_parse, fail-fast on
  // malformed input. For now just store and trust the caller.
  _caBundlePem = caBundlePem;
  return _caBundlePem.length() > 0;
}

void TLSContextService::attachToClient(WiFiClientSecure& client) {
  // Phase 1.1: client.setCACert(_caBundlePem.c_str()) +
  //   client.setCertificate(_clientCertPem) +
  //   client.setPrivateKey(_clientKeyPem) when client material
  //   is present, else just CA. For now no-op — caller's client
  //   stays in whatever mode it was constructed with.
  (void)client;
}

void TLSContextService::updateClientCert(const String& certPem,
                                         const String& keyPem) {
  _clientCertPem = certPem;
  _clientKeyPem = keyPem;
}

void TLSContextService::clearClientCert() {
  _clientCertPem = String();
  _clientKeyPem = String();
}
