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
  // Validate by feeding into a throwaway WiFiClientSecure — its
  // setCACert internally calls mbedtls_x509_crt_parse and fails
  // gracefully (returns false, but more importantly doesn't crash)
  // on malformed PEM. We don't actually keep the throwaway client;
  // it's purely a syntax check before committing to _caBundlePem.
  if (caBundlePem.length() == 0) {
    _caBundlePem = String();
    return false;
  }
  // Trust caller for now (lib doesn't surface a return code from
  // setCACert without performing a real handshake). Phase 1.5+ will
  // add a "preflight verify" using a real outbound connection and
  // either fall back to old CA or surface the error to UI.
  _caBundlePem = caBundlePem;
  return true;
}

void TLSContextService::attachToClient(WiFiClientSecure& client) {
  // arduino-esp32 NetworkClientSecure stores the const char* it
  // receives — does NOT copy. _caBundlePem / _clientCertPem /
  // _clientKeyPem are members of `this`, lifetime-tied to App
  // singleton, so the pointers are safe for the client's lifetime
  // as long as TLSContextService outlives the client. Mid-session
  // cert swap (Phase 4) creates a new String → if the client is
  // still using the old PEM the underlying char* might be freed →
  // we keep the OLD String alive in `_prevClient*` for the caller's
  // current handshake; new clients pick up the rotated material.
  if (_caBundlePem.length() > 0) {
    client.setCACert(_caBundlePem.c_str());
  } else {
    // No CA loaded yet (pre-enrollment) — caller must opt into
    // setInsecure() if they really need to talk to a server. We do
    // NOT setInsecure() ourselves so that "forgot to enroll" never
    // silently turns into a plaintext channel.
  }
  if (_clientCertPem.length() > 0 && _clientKeyPem.length() > 0) {
    client.setCertificate(_clientCertPem.c_str());
    client.setPrivateKey(_clientKeyPem.c_str());
  }
}

void TLSContextService::updateClientCert(const String& certPem,
                                         const String& keyPem) {
  // Stash previous PEM strings so already-attached clients keep a
  // valid backing buffer through their CURRENT handshake. Newly
  // attached clients (from this call onwards) get the new material.
  // Two-deep history is sufficient because by the time a third
  // rotation happens, any client from the first rotation has
  // long since closed.
  _prevPrevClientCertPem = std::move(_prevClientCertPem);
  _prevPrevClientKeyPem  = std::move(_prevClientKeyPem);
  _prevClientCertPem     = std::move(_clientCertPem);
  _prevClientKeyPem      = std::move(_clientKeyPem);

  _clientCertPem = certPem;
  _clientKeyPem  = keyPem;
}

void TLSContextService::clearClientCert() {
  _prevPrevClientCertPem = std::move(_prevClientCertPem);
  _prevPrevClientKeyPem  = std::move(_prevClientKeyPem);
  _prevClientCertPem     = std::move(_clientCertPem);
  _prevClientKeyPem      = std::move(_clientKeyPem);

  _clientCertPem = String();
  _clientKeyPem  = String();
}
