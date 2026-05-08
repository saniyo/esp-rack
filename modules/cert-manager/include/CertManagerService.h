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

// Phase 4b — gray-zone recovery polling endpoint. Hit when the
// device's cert is dead but it still has a recovery_token from a
// previous successful enrollment.
#ifndef FACTORY_MOTHERSHIP_RECOVER_URL
#define FACTORY_MOTHERSHIP_RECOVER_URL "https://mothership.local:8443/api/v1/recover"
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

  // ── Persisted endpoint config ──
  // Mothership enrollment URL. Configurable at runtime so operator
  // can repoint the device at staging / dev mock server without a
  // reflash. Defaulted to FACTORY_MOTHERSHIP_ENROLL_URL on first boot.
  String  mothership_url{FACTORY_MOTHERSHIP_ENROLL_URL};
  // Phase 4b — gray-zone recovery polling URL. Defaulted to the
  // factory value, persists across reboot so a device that goes
  // offline → cert expires can come back online and start polling
  // /recover on its own without operator intervention beyond the
  // server-side approve click.
  String  recover_url{FACTORY_MOTHERSHIP_RECOVER_URL};

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

  // Phase 4b — gray-zone recovery. Spawns a polling task that hits
  // recover_url every RECOVERY_POLL_INTERVAL_S until the server
  // returns approved=true with a fresh cert. Works without mTLS —
  // cert is dead so we can only do server-side TLS (CA pin from
  // the persisted ca_bundle_pem so we still verify it's the right
  // mothership). Operator on the server side approves the request
  // via /api/v1/admin/recover/approve/<deviceId>.
  //
  // Returns true if poll task was spawned, false if already
  // running, no recovery_token persisted (means full re-enroll
  // needed), or no recover_url. Phase 4b iteration 1 is operator-
  // triggered via UI button — phase iteration 2 will auto-trigger
  // on boot when refreshRuntimeState detects state == GrayZone.
  bool beginRecovery();

  // ICertProvider
  State state() const override { return _state.runtime_state; }
  bool hasValidCert() const override {
    return _state.runtime_state == State::Ready;
  }
  int32_t daysUntilExpiry() const override;
  const char* subjectCN() const override { return _state.subject_cn.c_str(); }
  const char* serialHex() const override { return _state.serial_hex.c_str(); }
  bool rotate(const String& renewUrl) override;

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

  // ── Phase 4a — proactive rotation ──
  //
  // Same task pattern as enrollment but a separate handle so a
  // rotate doesn't block / shadow an enrollment retry. Single in-
  // flight rotate at a time; second rotate request while one is
  // running is silently dropped (caller's `rotate()` returns false).
  TaskHandle_t _rotateTask{nullptr};
  static void  rotateTaskTramp(void* arg);
  void         runRotation(const String& renewUrl);

  // POST a new CSR through mTLS to `renewUrl` using the EXISTING
  // device cert as client identity (no bootstrap token — server
  // accepts the request because the mTLS handshake already proves
  // we're the cert's holder). On success populates the same three
  // outputs as enrollment minus recovery_token (which is rotation-
  // invariant — server doesn't re-issue it).
  bool postCsrToRenew(const String& csrPem,
                       const String& renewUrl,
                       String& outCertPem,
                       String& outCaBundlePem);

  // ── Phase 4b — gray-zone recovery ──
  //
  // Polling task that hits recover_url at RECOVERY_POLL_INTERVAL_S
  // cadence (60s) carrying {deviceId, recovery_token,
  // lastKnownSerial}. Runs WITHOUT mTLS (cert is dead) — uses
  // setInsecure on the WiFiClientSecure since we can't even verify
  // the server's cert against our existing CA when the entire
  // cert.json might be lost. Production hardens this by bundling a
  // bootstrap CA into firmware that signs the recover endpoint's
  // cert.
  //
  // Server returns either {approved: false, status: "pending|...} or
  // {approved: true, cert_pem, ca_bundle_pem} — the latter triggers
  // an atomic-swap into the TLS context (same path as rotate) and
  // exits GrayZone state.
  static constexpr uint32_t RECOVERY_POLL_INTERVAL_S = 60;
  TaskHandle_t _recoveryTask{nullptr};
  static void  recoveryTaskTramp(void* arg);
  void         runRecoveryLoop();
  bool         postRecoveryRequest(String& outCertPem,
                                    String& outCaBundlePem,
                                    bool& outApproved);

  // Candidate keypair generated on the FIRST recovery poll; reused
  // across subsequent polls so the CSR (and thus the eventual
  // signed cert) keeps matching the same private key. Cleared
  // after successful apply. NEVER persisted — lives in RAM only;
  // a reboot mid-recovery loses it and recovery starts fresh next
  // boot (which is fine — server-side pending request becomes a
  // dangling reference that operator's admin UI can clean up).
  String _candidateKeyPem;
};

#endif  // CertManagerService_h
