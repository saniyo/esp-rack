#pragma once
#ifndef ESPRACK_TLS_CONTEXT_SERVICE_H
#define ESPRACK_TLS_CONTEXT_SERVICE_H

// TLSContextService — concrete implementation of ITLSProvider.
//
// Phase 1.0 SKELETON: all methods are no-op stubs. The class
// compiles, App can hold an instance, modules can call its methods —
// nothing actually happens. Phase 1.1 fills in:
//   - PEM-to-mbedtls-cert parsing for CA + client chain
//   - WiFiClientSecure::setCACert / setCertificate / setPrivateKey
//     plumbing
//   - thread-safe cert swap (mutex guarding the in-RAM PEM strings)
//
// Lives in lib/ESPRack (not a module) because:
//   1. It's a framework primitive — multiple modules depend on it,
//      so it can't sit inside any one module.
//   2. App holds the singleton via late-bind setter (same pattern as
//      SecurityManager / PresenceService) — consumers do
//      app->tls()->attachToClient(client).
//
// CA bundle source is the device's bundled-in PEM (compiled-in via
// CA_BUNDLE_PEM extern, populated from a CMake-time bundle generation
// in Phase 1.1). Client cert+key come from CertManagerService at
// runtime via updateClientCert.

#include "ITLSProvider.h"

class TLSContextService : public ITLSProvider {
 public:
  TLSContextService();
  ~TLSContextService() override;

  // Loads a CA bundle (PEM concat of allowed-server roots) into the
  // active context. Phase 1.0 stub: store + return — no parsing yet.
  // Returns true on parse-success; false on malformed PEM.
  bool loadCaChain(const String& caBundlePem);

  // ITLSProvider
  bool hasCaChain() const override { return _caBundlePem.length() > 0; }
  bool hasClientCert() const override {
    return _clientCertPem.length() > 0 && _clientKeyPem.length() > 0;
  }
  void attachToClient(WiFiClientSecure& client) override;
  void updateClientCert(const String& certPem, const String& keyPem) override;
  void clearClientCert() override;

 private:
  // PEM-encoded material — kept as String for simplicity. arduino-
  // esp32 NetworkClientSecure stores raw const char* (not a copy),
  // so PEM Strings must outlive any WiFiClientSecure they were
  // attached to. _prev* / _prevPrev* slots keep the previous one or
  // two PEM blobs alive across cert swaps so already-running
  // handshakes don't dereference freed memory; by the third swap
  // any old client is long-since closed.
  String _caBundlePem;
  String _clientCertPem;
  String _clientKeyPem;
  String _prevClientCertPem;
  String _prevClientKeyPem;
  String _prevPrevClientCertPem;
  String _prevPrevClientKeyPem;
};

#endif  // ESPRACK_TLS_CONTEXT_SERVICE_H
