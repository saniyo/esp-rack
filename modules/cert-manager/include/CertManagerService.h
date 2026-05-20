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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define CERT_MANAGER_FILE      "/config/cert.json"
#define CERT_MANAGER_FORM_PATH "/rest/certManager"
#define CERT_MANAGER_WS_PATH   "/ws/certManager"

// Build-time default for the mothership enroll endpoint. Operator
// can override via the Settings tab at runtime; this is what the
// device tries on factory-reset / first boot if no override is
// stored. Mirrors how MQTT / AutoUpdate take their factory defaults
// (see FACTORY_MQTT_HOST in modules/mqtt/include/MqttSettingsService.h).
#ifndef FACTORY_MOTHERSHIP_ENROLL_URL
#define FACTORY_MOTHERSHIP_ENROLL_URL "https://mothership.local:8443/api/v1/enroll"
#endif

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

  // ── Persisted PKI endpoint override ──
  // When EMPTY (default): cert-manager follows the active Mothership
  //                       profile (app->mothershipProfile()->...);
  //                       enroll and recover URLs come from the same
  //                       base URL as checkin.
  // When NON-EMPTY:       cert-manager uses this base URL ("https://
  //                       host:port", no trailing /) for enroll +
  //                       recovery, decoupled from the Mothership
  //                       profile. Lets the operator run a separate
  //                       CA server (e.g. air-gapped internal CA)
  //                       while keeping the regular Mothership host
  //                       for check-in / commands.
  // Endpoint paths are fixed per server contract (/api/v1/enroll,
  // /api/v1/recover) and appended at request time.
  String  pki_base_url;

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

// Forward-declare so we don't have to pull the wireguard module's
// public header just to know the type of the optional pointer.
class IWireguardProvider;
// Forward decl — App.h is heavy; we only need the pointer here.
namespace ESPRack { class App; }

class CertManagerService : public StatefulService<CertManagerSettings>,
                           public ICertProvider {
 public:
  CertManagerService(ConfigManager* cfgMgr, ITLSProvider* tls);

  // Wires the framework App into the service so PKI URL resolution
  // can fall back to the active Mothership profile when the operator
  // hasn't pinned a separate pki_base_url. Called by the consuming
  // Module after MothershipModule installs.
  void setApp(ESPRack::App* app) { _app = app; }

  void registerManifest(WebManager* web);
  void begin();
  void loop();

  // Phase WG.3 — let the enroll flow include the device's WG
  // public key in the CSR payload so the mothership can pre-allocate
  // a tunnel IP and add the peer at enrollment time. Optional.
  void setWireguardProvider(IWireguardProvider* wg) { _wg = wg; }

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
  // Optional — set via setWireguardProvider after WireGuardModule
  // installs (priorities 12 vs 14, but the App pointer is stable
  // throughout, so plain late-bind is enough).
  IWireguardProvider*                      _wg{nullptr};
  ESPRack::App*                            _app{nullptr};

  // Resolve enroll / recover URLs at request time, picking either
  // the operator-pinned pki_base_url (Settings tab) or the active
  // Mothership profile (when pki_base_url is empty). Returns empty
  // when neither path resolves to a usable URL.
  String effectiveEnrollUrl() const;
  String effectiveRecoverUrl() const;

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

  // ── Phase 1.5 — Enrollment HTTPS POST ──
  //
  // Spawn a FreeRTOS task that runs the full enrollment flow off the
  // AsyncWebServer thread (the action handler returns 200 immediately
  // so UI doesn't block). Single-task in-flight only; if a task is
  // already running, returns false. Caller's `bootstrapToken` is
  // captured into a heap-allocated argument struct that the task
  // owns and frees.
  bool kickEnrollment(const String& bootstrapToken);

  // The task body — runs on its own stack, calls back into the
  // service for state mutation. Trampoline / static so it can be
  // passed to xTaskCreate.
  static void enrollTaskTramp(void* arg);
  void runEnrollment(const String& bootstrapToken);

  // POST CSR to mothership_url with bootstrap token. Returns true
  // and populates outputs on 2xx response. Server response shape:
  //   {"cert_pem": "...", "ca_bundle_pem": "...", "recovery_token": "hex..."}
  // First-enrollment uses setInsecure() (we don't have a CA pinned
  // yet) — Phase 1.5 wires this with a TODO to switch to bundled
  // bootstrap CA in production builds. Once enrolled, subsequent
  // mothership calls use mTLS via the freshly-saved client cert and
  // setCACert(ca_bundle_pem).
  bool postCsrToMothership(const String& csrPem,
                            const String& bootstrapToken,
                            String& outCertPem,
                            String& outCaBundlePem,
                            String& outRecoveryToken);

  // Parse a freshly-received cert PEM to extract serial + notAfter
  // for state display. Uses mbedtls_x509_crt_parse + crt.serial.p +
  // crt.valid_to. Returns true on parse-success.
  bool parseCertMetadata(const String& certPem,
                          String& outSerialHex,
                          uint32_t& outNotAfterTs);

  TaskHandle_t _enrollTask{nullptr};
};

#endif  // CertManagerService_h
