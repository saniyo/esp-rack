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

// Factory bootstrap token — one-time shared secret baked into the
// firmware at build time. Empty by default; set via a build_flag
// in platformio.ini for production fleets that want all devices
// to ship with the same out-of-the-box token:
//
//   -D FACTORY_CERT_BOOTSTRAP_TOKEN=\"f3d29b1a…\"
//
// On first boot (no cert.json yet) the token is loaded into the
// persisted CertManagerSettings.bootstrap_token field. The auto-
// enroll loop sends it as `Authorization: Bearer …` on every
// /enroll attempt — server's path 2 fast-path signs + auto-trusts
// when it matches. On successful enrollment the token is wiped
// (in RAM AND on disk) so it cannot be reused; if a re-enroll is
// ever needed it falls back to the operator-approval path 6.
//
// Devices flashed without the build flag start with an empty
// token and rely purely on operator approval (path 6 / grey list).
#ifndef FACTORY_CERT_BOOTSTRAP_TOKEN
#define FACTORY_CERT_BOOTSTRAP_TOKEN ""
#endif

// Legacy FACTORY_MOTHERSHIP_ENROLL_URL / FACTORY_MOTHERSHIP_RECOVER_URL
// have been removed — cert-manager no longer carries its own URL
// fields. Both enroll and recover URLs are derived at request time
// via effectiveEnrollUrl() / effectiveRecoverUrl():
//   * If operator set Settings tab's "PKI Base URL" → that prefix
//     + /api/v1/enroll | /api/v1/recover
//   * Otherwise → active Mothership profile's base URL + same paths
// One Settings/Profiles edit on the Mothership tab repoints the
// whole PKI relationship atomically.

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

  // ── Persisted one-time bootstrap token (shared-secret fast-path) ──
  // Survives reboot so a fleet can be flashed once and auto-enroll
  // without operator interaction even across power cycles. WIPED
  // on first successful enrollment (single-use): the next time the
  // device needs to re-enroll (factory-reset, operator unbind, …)
  // it falls back to the deviceId / grey-zone path. Encrypted on
  // disk by SecretsVault — marked as a secret field in buildForm
  // so plaintext never lands in /config/cert.json.
  // Default value comes from the FACTORY_CERT_BOOTSTRAP_TOKEN
  // build-flag; empty when the flag is not set at compile time.
  String  bootstrap_token{FACTORY_CERT_BOOTSTRAP_TOKEN};

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
  // /api/v1/recover) and appended at request time. recover_url is
  // NOT a separate field — Phase 4b uses effectiveRecoverUrl().
  String  pki_base_url;

  // ── Auto-poll telemetry (runtime, not persisted) ──
  // Updated by the enrollment task on every /enroll round-trip;
  // exposed read-only on the Enrollment tab so the operator can
  // see at a glance that the device is alive and waiting. Reset
  // on every reboot — first poll lands within seconds of WiFi up.
  uint32_t enroll_poll_count{0};   // attempts since boot
  uint32_t enroll_last_poll_s{0};  // seconds-since-boot of last attempt
  String   enroll_last_reason;     // human-readable last server reply

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

  // Phase 4b — gray-zone recovery. Spawns a polling task that hits
  // effectiveRecoverUrl() every RECOVERY_POLL_INTERVAL_S until the
  // server returns approved=true with a fresh cert. Works without
  // mTLS — cert is dead so we can only do server-side TLS (CA pin
  // from the persisted ca_bundle_pem so we still verify it's the
  // right mothership). Operator on the server side approves the
  // request via /api/v1/admin/recover/approve/<deviceId>.
  //
  // Returns true if poll task was spawned, false if already
  // running, no recovery_token persisted (means full re-enroll
  // needed), or no recover URL resolvable. Phase 4b iteration 1 is
  // operator-triggered via UI button — phase iteration 2 will
  // auto-trigger on boot when refreshRuntimeState detects state
  // == GrayZone.
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
  // mutation. Runs in begin(), after successful enroll/rotate, and
  // periodically from loop() (so the NTP-race GrayZone gets cleared
  // once SNTP brings the wall clock forward).
  void refreshRuntimeState();

  // Last millis() the periodic refresh fired — gates the loop() body
  // to 1-per-60s so we don't burn cycles on a no-op state recheck.
  uint32_t _lastStateRefreshMs{0};

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

  // ── Approval-driven enrollment ──
  //
  // Auto-spawned from begin() when state != Ready. The task waits
  // for WiFi + a resolvable enroll URL, generates the keypair + CSR
  // once, then loops postEnrollOnce until either:
  //   * Approved   → save cert, update TLS context, state → Ready,
  //                   task exits
  //   * Blacklisted → state → EnrollmentFailed, task exits (manual
  //                   wipe or operator-side unblock required)
  //   * Externally bumped to Ready by another path (rotate, etc.)
  // Pending / network / server errors just delay and retry.
  //
  // Returns false if a task is already running (begin() can call
  // safely without an explicit guard; task self-clears the handle
  // before vTaskDelete).
  bool kickEnrollment();

  // The task body — runs on its own stack, calls back into the
  // service for state mutation. Trampoline / static so it can be
  // passed to xTaskCreate.
  static void enrollTaskTramp(void* arg);
  void runEnrollmentLoop();

  // Outcome of one /enroll round-trip. Drives loop scheduling:
  //   * Approved   — stop (success)
  //   * Pending    — re-poll after PENDING_INTERVAL_S
  //   * Blacklisted— stop (failure, operator must act)
  //   * NetworkFail/ServerError — re-poll after ERROR_INTERVAL_S
  enum class EnrollResult {
    Approved,
    Pending,
    Blacklisted,
    NetworkFail,
    ServerError,
  };

  // One /enroll attempt. Sends {deviceId, csr_pem [, wg_pubkey]} —
  // when state.bootstrap_token is non-empty also adds the matching
  // Authorization: Bearer header (server's path 2 fast-path; auto-
  // trust on token match). Empty token → no header, server path 6
  // parks the device in PENDING_ENROLLMENTS for operator approval.
  // Populates the three PEM outputs ONLY on Approved; populates
  // outReason on every result for the UI telemetry field.
  EnrollResult postEnrollOnce(const String& csrPem,
                              String& outCertPem,
                              String& outCaBundlePem,
                              String& outRecoveryToken,
                              String& outReason);

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
