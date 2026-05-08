#pragma once
#ifndef CertManagerService_h
#define CertManagerService_h

// CertManagerService — owns device PKI lifecycle. Phase 1.0
// SKELETON: state struct, ICertProvider impl with hard-coded
// "NeedsEnrollment", begin() loads from disk if cert.json exists.
// Crypto / network code arrives in subsequent commits:
//
//   Phase 1.1 — TLSContextService PEM-to-mbedtls plumbing
//   Phase 1.2 — ConfigDelegate persistence + UI tab (storage shape only)
//   Phase 1.3 — ECDSA-P256 keypair generation
//   Phase 1.4 — CSR build (mbedtls_x509write_csr)
//   Phase 1.5 — Enrollment HTTPS POST (server-side TLS, bootstrap token)
//   Phase 1.6 — Mock server for end-to-end tests
//   Phase 1.7 — Atomic verify-then-swap on cert load
//
// Beyond Phase 1: rotate() (Phase 4a) + gray-zone recovery (Phase 4b).

#include <StatefulService.h>
#include <ConfigManager.h>
#include <ConfigDelegate.h>
#include <FormBuilder.h>
#include <WebFeatureDelegate.h>

#include "ICertProvider.h"

#include <Arduino.h>

#define CERT_MANAGER_FILE      "/config/cert.json"
#define CERT_MANAGER_FORM_PATH "/rest/certManager"
#define CERT_MANAGER_WS_PATH   "/ws/certManager"

class WebManager;
class ITLSProvider;

// Persisted shape — three secret PEM blobs + housekeeping. PEM is
// already text-safe; SecretsVault encrypts via the existing
// `secret;` field option in buildForm. Loaded once on boot, atomic
// swap in Phase 1.7. Fields stay empty until first enrollment.
struct CertManagerSettings {
  // ── Persisted secrets ──
  String device_cert_pem;   // X.509 PEM (~400 B for ECDSA-P256)
  String device_key_pem;    // EC PRIVATE KEY PEM (~250 B)
  String ca_bundle_pem;     // root CA(s) the device trusts on outbound HTTPS

  // ── Persisted housekeeping ──
  String  serial_hex;       // for operator-side audit ("0a:5f:..")
  String  subject_cn;       // typically "device-<MAC>"
  uint32_t not_after_ts;    // unix time of cert expiry (0 = no cert)

  // ── Persisted recovery ──
  // Random 32-byte secret generated at first successful enroll.
  // Used by gray-zone recovery flow (Phase 4b) when cert is lost
  // but device can still authenticate as itself. Persisted hex.
  // Empty until Phase 1.5 sets it on first enroll.
  String  recovery_token;

  // ── Runtime (not persisted) ──
  // Bootstrap token for first enrollment. Operator types it in the
  // UI; cleared after successful enroll. NEVER persisted to disk
  // (lives only in RAM during the enrollment window).
  String  bootstrap_token;

  // Derived display state — recomputed in begin()/onCertChanged().
  ICertProvider::State runtime_state{ICertProvider::State::Uninitialised};
  String  status_label{"Uninitialised"};

  static void readConfig(CertManagerSettings& s, JsonObject& root);
  static StateUpdateResult update(JsonObject& root, CertManagerSettings& s);
  static void buildForm(CertManagerSettings& s, JsonObject& root);
  static void staRead(CertManagerSettings& s, JsonObject& root);
  static StateUpdateResult staUpd(JsonObject& root, CertManagerSettings& s);
};

class CertManagerService : public StatefulService<CertManagerSettings>,
                           public ICertProvider {
 public:
  CertManagerService(ConfigManager* cfgMgr, ITLSProvider* tls);

  void registerManifest(WebManager* web);
  void begin();
  void loop();

  // ICertProvider
  State state() const override { return _state.runtime_state; }
  bool hasValidCert() const override {
    return _state.runtime_state == State::Ready;
  }
  int32_t daysUntilExpiry() const override;
  const char* subjectCN() const override { return _state.subject_cn.c_str(); }
  const char* serialHex() const override { return _state.serial_hex.c_str(); }

 private:
  ConfigDelegate<CertManagerSettings>      _cfg;
  WebFeatureEntry<CertManagerSettings>*    _feature{nullptr};
  ITLSProvider*                            _tls{nullptr};

  // Recompute runtime_state from on-disk fields after every load /
  // mutation. Runs in begin() and after successful enroll/rotate.
  void refreshRuntimeState();

  // ── Phase 1.3+1.4 crypto helpers (mbedtls bindings) ──
  //
  // Generate a fresh ECDSA-P256 keypair. Outputs PEM strings via the
  // out parameters (private key as "-----BEGIN EC PRIVATE KEY-----"
  // ~250 B; public key not needed externally — embedded in the CSR).
  // Uses esp_random as the RNG seed (hardware on every supported
  // ESP32 family). Returns true on success.
  //
  // Defined here so Phase 4 rotate() and Phase 1.5 enroll() share
  // the same primitive; on non-ESP host builds the body is a stub
  // that fails predictably.
  bool generateEcdsaKeyPair(String& outKeyPem);

  // Build a PKCS#10 CSR from `keyPem` with subject "CN=device-<MAC>".
  // Signed with SHA-256. Returns the CSR PEM (~480 B) on success.
  // The CSR carries the public half of `keyPem` and a signature
  // proving possession; mothership signs it into a real cert.
  bool buildCsr(const String& keyPem, String& outCsrPem);

  // Helper: derive the canonical subject CN from this device's
  // base MAC ("device-aabbccddeeff" lowercased, no separators).
  // Used by buildCsr; exposed for the enroll-status display.
  String deviceSubjectCN() const;
};

#endif  // CertManagerService_h
