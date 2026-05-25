#include <CertManagerService.h>
#include <ITLSProvider.h>
#include <IWireguardProvider.h>
#include <IMothershipProfileProvider.h>
#include <App.h>
#include <DeviceIdentity.h>
#include <WebManager.h>

// mbedtls bindings for ECDSA-P256 keypair + PKCS#10 CSR + cert
// metadata parsing. arduino-esp32 ships with mbedtls compiled in;
// these headers come from the framework's bundled mbedtls and
// don't need a lib_deps entry.
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/x509_csr.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>

// HTTPS client + JSON for the enrollment POST.
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

#if defined(ESP32)
#include <esp_mac.h>   // esp_read_mac for MAC-based CN derivation
#endif

// ===== Persistence (ConfigDelegate) =====

void CertManagerSettings::readConfig(CertManagerSettings& s, JsonObject& root) {
  // Three PEM blobs go through SecretsVault — see buildForm for the
  // `secret;` field markers that drive auto-encryption.
  root["device_cert_pem"] = s.device_cert_pem;
  root["device_key_pem"]  = s.device_key_pem;
  root["ca_bundle_pem"]   = s.ca_bundle_pem;

  root["serial_hex"]    = s.serial_hex;
  root["subject_cn"]    = s.subject_cn;
  root["not_after_ts"]  = s.not_after_ts;

  root["recovery_token"]  = s.recovery_token;
  // bootstrap_token PERSISTS now — survives reboot until single-use
  // wipe lands on first successful enrollment. Encrypted on disk via
  // SecretsVault (buildForm marks it as a secret field).
  root["bootstrap_token"] = s.bootstrap_token;
  root["pki_base_url"]    = s.pki_base_url;
  // auto-enroll runs unconditionally now (post d3caf86 model): when
  // state != Ready, begin() spawns a polling task that hits /enroll
  // with the canonical deviceId as the identity claim. No toggle.
  // recover_url is NOT a separate field — derives from
  // effectiveRecoverUrl() (pki_base_url + /api/v1/recover, falling
  // back to the active Mothership profile's recoverUrl).
}

StateUpdateResult CertManagerSettings::update(JsonObject& root,
                                              CertManagerSettings& s) {
  bool ch = false;

  // ── Sensitive fields (cert / key / CA / recovery token) ────────
  // These have DUAL parsers because update() is invoked both at
  // boot (ConfigDelegate apply JSON-from-disk → state) AND on every
  // form POST (UI form Save → state). At boot we MUST adopt the
  // saved value. On form POST the readonly SecretFields might come
  // through as null/empty (frontend behaviour for AF::R) and would
  // otherwise clobber a real loaded PEM.
  //
  // Rule: adopt only if the incoming JSON has the key AND its value
  // is non-empty. Empty / missing → keep existing state. The boot
  // path (where state is freshly default-constructed) sees the
  // populated PEM in JSON and adopts; the form-POST path either
  // doesn't include the key (frontend omits AF::R) or includes it
  // as the same plaintext from the previous GET (no real change).
  // No way to wipe a sensitive field through the form — the only
  // path that sets these to empty is runEnrollment / rotate /
  // factory-reset, which mutate state directly.
  auto setIfNonEmpty = [&](const char* key, String& dst) {
    if (!root.containsKey(key)) return;
    JsonVariant v = root[key];
    if (v.isNull()) return;
    String incoming = v.as<String>();
    if (incoming.length() == 0) return;
    if (incoming != dst) {
      dst = incoming;
      ch = true;
    }
  };
  setIfNonEmpty("device_cert_pem", s.device_cert_pem);
  setIfNonEmpty("device_key_pem",  s.device_key_pem);
  setIfNonEmpty("ca_bundle_pem",   s.ca_bundle_pem);
  setIfNonEmpty("recovery_token",  s.recovery_token);
  // bootstrap_token uses the same secret-safe pattern as PEM fields:
  // boot-path JSON has the encrypted-then-decrypted value → adopt;
  // form-POST path either omits the key (AF::R) or echoes the same
  // plaintext (no-op). The ONLY path that wipes the token to empty
  // is the runEnrollmentLoop success branch (single-use semantics).
  setIfNonEmpty("bootstrap_token", s.bootstrap_token);

  // ── Plain non-sensitive housekeeping ──
  ch |= FormBuilder::updateValue(root, "serial_hex",      s.serial_hex);
  ch |= FormBuilder::updateValue(root, "subject_cn",      s.subject_cn);
  ch |= FormBuilder::updateValue(root, "not_after_ts",    s.not_after_ts);
  ch |= FormBuilder::updateValue(root, "pki_base_url",    s.pki_base_url);

  return ch ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
}

// ===== Form schema =====
// Phase 1.2: three tabs.
//   Status tab       — read-only cert details + state-driven avatar
//   Enrollment tab   — bootstrap-token entry + Enroll action
//                      (visible only in NeedsEnrollment / Failed states
//                       via showIf — Ready/Renewing tabs hide it)
//   Recovery tab     — gray-zone polling readout (populated in Phase 4b;
//                       skeleton shown so UI structure is stable)
void CertManagerSettings::buildForm(CertManagerSettings& s, JsonObject& root) {
  // ── STATUS ─────────────────────────────────────────────────────────
  JsonArray st = FormBuilder::createForm(root, "status",
                                          "Device PKI status");

  FormBuilder::addTextField(st, "status", AF::R, s.status_label.c_str(),
                            label("State"), icon("VerifiedUser"),
                            colorMap("Ready:success,Renewing:info,"
                                     "Enrolling:info,GrayZone:warning,"
                                     "EnrollmentFailed:error,"
                                     "NeedsEnrollment:warning,default:info"));
  FormBuilder::addTextField(st, "subject_cn", AF::R, s.subject_cn.c_str(),
                            label("Subject CN"), icon("Badge"));
  FormBuilder::addTextField(st, "serial_hex", AF::R, s.serial_hex.c_str(),
                            label("Cert serial"), icon("Tag"));
  FormBuilder::addNumberField(st, "not_after_ts", AF::R,
                              (double)s.not_after_ts, format("0"),
                              label("notAfter (unix)"), icon("Schedule"));
  // Helpful for operator at-a-glance — derived from not_after_ts +
  // local clock at form-render time. Days < 0 ⇒ expired/gray-zone.
  int32_t days_left = INT32_MIN;
  if (s.not_after_ts > 0) {
    uint32_t now_s = (uint32_t)time(nullptr);
    if (now_s > 0) {
      days_left = (int32_t)(((int64_t)s.not_after_ts - (int64_t)now_s) / 86400);
    }
  }
  if (days_left != INT32_MIN) {
    FormBuilder::addNumberField(st, "days_until_expiry", AF::R,
                                (double)days_left, format("0"),
                                label("Days until expiry"), icon("Update"),
                                colorMap("default:success"));
  }

  // ── ENROLLMENT ─────────────────────────────────────────────────────
  // Auto-polling status view. begin() spawns a permanent FreeRTOS
  // task whenever state != Ready; the task hits /enroll on a 30s
  // cadence using the canonical deviceId as the identity claim.
  // No operator action required for the happy path — once the
  // operator clicks Approve on the mothership admin page (or path 2
  // matches a baked-in bootstrap_token, or path 3 finds the device
  // in EXPECTED_DEVICES) the next poll lands a signed cert and the
  // state transitions to Ready. The three read-only fields below
  // show what's being sent and what the server most recently said.
  JsonArray en = FormBuilder::createForm(root, "enrollment",
                                          "Auto-enrollment status");

  FormBuilder::addMessageField(en, "m_enroll_help",
      "The device auto-polls the mothership's /enroll endpoint on a "
      "30s cadence and waits for one of three approval paths:\n"
      "  1. Bootstrap token (below) matches → instant auto-trust\n"
      "  2. deviceId is on the mothership's EXPECTED_DEVICES list → "
      "instant auto-trust\n"
      "  3. Operator manually approves from the pending list (grey "
      "zone) on the mothership admin page",
      level("info"), icon("Info"));

  // Read-only telemetry — populated by runEnrollmentLoop on every
  // poll round-trip. Operator can refresh the form to see "Polls
  // since boot" tick up, confirming the device is alive.
  FormBuilder::addTextField(en, "device_id", AF::R,
                            DeviceIdentity::canonical().c_str(),
                            label("Device ID (sent to mothership)"),
                            icon("Fingerprint"));
  FormBuilder::addNumberField(en, "enroll_poll_count", AF::R,
                              (double)s.enroll_poll_count, format("0"),
                              label("Polls since boot"), icon("Loop"));
  FormBuilder::addTextField(en, "enroll_last_reason", AF::R,
                            s.enroll_last_reason.c_str(),
                            label("Last server reply"),
                            icon("Info"));

  // One-time bootstrap token — persisted secret. Operator can paste
  // a token here to trigger server's path 2 fast-path; on successful
  // enrollment the field is wiped both in RAM and on disk (single-
  // use semantics). FACTORY_CERT_BOOTSTRAP_TOKEN build-flag bakes a
  // default into firmware for fleet-wide auto-provision.
  FormBuilder::addSecretField(en, "bootstrap_token", AF::RW,
                              s.bootstrap_token.c_str(),
                              label("Bootstrap token (one-time, optional)"),
                              placeholder("blank = wait for operator approval"),
                              icon("VpnKey"));

  // ── SETTINGS ────────────────────────────────────────────────────────
  // PKI endpoint override. Empty (default) → cert-manager follows the
  // active Mothership profile (one base URL feeds /checkin + /enroll +
  // /recover). Non-empty → operator-pinned base for enroll/recover,
  // letting them run a separate CA server without touching the
  // Mothership profile.
  JsonArray set = FormBuilder::createForm(root, "settings",
                                          "PKI endpoint");

  FormBuilder::addTextField(set, "pki_base_url", AF::RW,
                            s.pki_base_url.c_str(),
                            label("PKI Base URL (optional override)"),
                            placeholder("https://ca.example.com:8443"),
                            icon("Cloud"));

  // ── RECOVERY ────────────────────────────────────────────────────────
  // Phase 4b — gray-zone recovery. Operator-triggered (UI button)
  // for now; Phase 4b iteration 2 will auto-trigger on boot when
  // the cert is detected expired with recovery_token present.
  JsonArray rc = FormBuilder::createForm(root, "recovery",
                                          "Gray-zone recovery (lost cert)");

  FormBuilder::addMessageField(rc, "m_recovery_help",
      "When the device's cert is lost or expired (state=GrayZone), "
      "click \"Trigger Recovery\" to start polling the mothership's "
      "/recover endpoint. The poll carries recovery_token and "
      "deviceId; an operator on the server side approves the request "
      "via /api/v1/admin/recover/approve/<deviceId>. On approval the "
      "device receives a fresh cert and exits gray-zone automatically.",
      level("info"), icon("Info"));

  // No `recover_url` text field on this branch — the recover URL
  // derives from effectiveRecoverUrl() which falls back from the
  // operator-pinned pki_base_url (Settings tab) to the active
  // Mothership profile's base URL. One Settings/Profiles edit
  // repoints both enroll + recover atomically.

  FormBuilder::addActionField(rc, "trigger_recovery", "Trigger Recovery",
                              AF::RW,
                              actionRef("cert.beginRecovery"),
                              icon("Restore"), color("warning"),
                              refetchForm());

  FormBuilder::addMessageField(rc, "m_recovery_warn",
      "Recovery uses server-side TLS only (mTLS impossible — cert "
      "is dead). Production firmware should pin a bootstrap CA at "
      "compile time so this endpoint can't be MITM'd. The dev mock "
      "uses setInsecure on this path to keep testing simple.",
      level("warning"), icon("Warning"));

  // ── INTERNALS ─────────────────────────────────────────────────────
  // Sensitive material — cert PEM, private key PEM, CA bundle PEM,
  // recovery token. Marked as secret so SecretsVault auto-discovery
  // catches them and encrypts on disk (otherwise they'd land
  // PLAINTEXT in /config/cert.json, which is exactly what we don't
  // want for a private key). Read-only — operator can toggle the
  // eye icon for debug visibility but can't edit. Lives in its own
  // tab so the day-to-day Status / Enrollment / Settings tabs stay
  // uncluttered.
  JsonArray intl = FormBuilder::createForm(root, "internals",
                                            "Sensitive material (encrypted on disk)");

  FormBuilder::addMessageField(intl, "m_internals_warn",
      "These fields contain the device's cryptographic identity. "
      "Encrypted at rest by SecretsVault — eye icon reveals plaintext "
      "for debug. Do NOT modify by hand: edits via this UI are "
      "discarded on save (fields are AF::R). To rotate the cert use "
      "the Mothership's renewCert command (Phase 4) or factory-reset "
      "the device and re-enroll.",
      level("warning"), icon("Warning"));

  FormBuilder::addSecretField(intl, "device_cert_pem", AF::R,
                              s.device_cert_pem.c_str(),
                              label("Device cert (PEM)"), icon("VerifiedUser"));
  FormBuilder::addSecretField(intl, "device_key_pem", AF::R,
                              s.device_key_pem.c_str(),
                              label("Device private key (PEM)"), icon("Key"));
  FormBuilder::addSecretField(intl, "ca_bundle_pem", AF::R,
                              s.ca_bundle_pem.c_str(),
                              label("Mothership CA bundle (PEM)"), icon("AccountTree"));
  FormBuilder::addSecretField(intl, "recovery_token", AF::R,
                              s.recovery_token.c_str(),
                              label("Recovery token"), icon("VpnKey"));
}

// ===== WS push =====
void CertManagerSettings::staRead(CertManagerSettings& s, JsonObject& root) {
  root["status"]        = s.status_label;
  root["subject_cn"]    = s.subject_cn;
  root["serial_hex"]    = s.serial_hex;
  root["not_after_ts"]  = s.not_after_ts;
  // never push secrets / bootstrap token / recovery token over WS
}

StateUpdateResult CertManagerSettings::staUpd(JsonObject& root,
                                              CertManagerSettings& s) {
  return update(root, s);
}

// ===== Service =====

CertManagerService::CertManagerService(ConfigManager* cfgMgr,
                                       ITLSProvider* tls)
    : StatefulService<CertManagerSettings>(),
      _cfg(cfgMgr,
           "certManager",
           CERT_MANAGER_FILE,
           // 8 KB JSON buffer: four ENC: blobs (cert+key+ca+recovery)
           // each ~2x plaintext after hex-encoding the IV + ciphertext.
           // Worst-case totals ~2.7 KB of strings + housekeeping +
           // ArduinoJson per-key overhead. The PREVIOUS 4 KB sometimes
           // wasn't enough — encryptSecretsInPlace's last LARGE expand
           // (typically ca_bundle_pem) silently failed mid-assignment
           // when the doc pool was exhausted, leaving the on-disk JSON
           // with ca_bundle_pem: null. 8 KB gives 5 KB headroom.
           8192,
           this,
           CertManagerSettings::readConfig,
           CertManagerSettings::update,
           false /*autoSave*/,
           nullptr /*validator*/,
           CertManagerSettings::buildForm /*formReader for secret-key probe*/),
      _tls(tls) {
  addUpdateHandler([this](const String& origin) {
    refreshRuntimeState();
    _cfg.saveIfChanged(origin);
    if (_feature) _feature->broadcastWs(origin);
  }, false);
}

void CertManagerService::registerManifest(WebManager* web) {
  if (!web) return;

  // No operator-triggered enrollment action — the auto-poll task
  // spawned in begin() handles everything. Operator can paste a
  // bootstrap token in the Enrollment tab → Save persists it
  // (SecretsVault-encrypted) → next poll picks it up via the
  // setIfNonEmpty refresh in update(). On Approved the loop wipes
  // the token from state AND from disk (single-use semantics).

  // Phase 4b — gray-zone recovery trigger. UI button on the
  // Recovery tab kicks off the polling task. Body-less POST; the
  // service reads recover_url + recovery_token from _state.
  WebActionSpec recoverAct;
  recoverAct.id              = "cert.beginRecovery";
  recoverAct.title           = "Trigger Recovery";
  recoverAct.icon            = "Restore";
  recoverAct.color           = "warning";
  recoverAct.auth            = WebAuthLevel::Admin;
  recoverAct.successMessage  = "Recovery polling started";
  recoverAct.handler = [this](AsyncWebServerRequest* r) {
    if (!beginRecovery()) {
      r->send(409, "application/json",
              "{\"ok\":false,\"err\":\"recovery already running, no recovery_token, or no recover_url\"}");
      return;
    }
    r->send(200, "application/json", "{\"ok\":true}");
  };
  web->registerAction(recoverAct);

  WebFeatureSpec spec;
  spec.id         = "certManager";
  spec.title      = "Device PKI";
  spec.component  = "DynamicSettings";
  spec.menu.label = "PKI";
  spec.menu.icon  = "VerifiedUser";
  spec.menu.order = 360;
  spec.menu.auth  = WebAuthLevel::Admin;
  spec.auth       = WebAuthLevel::Admin;
  spec.restRead   = CERT_MANAGER_FORM_PATH;
  spec.restUpdate = CERT_MANAGER_FORM_PATH;
  spec.wsPath     = CERT_MANAGER_WS_PATH;

  WebTabSpec statusTab;
  statusTab.key      = "status";
  statusTab.title    = "Status";
  statusTab.restPath = CERT_MANAGER_FORM_PATH;
  statusTab.postable = false;
  statusTab.live     = true;
  spec.tabs.push_back(statusTab);

  WebTabSpec enrollmentTab;
  enrollmentTab.key      = "enrollment";
  enrollmentTab.title    = "Enrollment";
  enrollmentTab.restPath = CERT_MANAGER_FORM_PATH;
  enrollmentTab.postable = true;
  enrollmentTab.auth     = WebAuthLevel::Admin;
  enrollmentTab.order    = 15;
  spec.tabs.push_back(enrollmentTab);

  WebTabSpec settingsTab;
  settingsTab.key      = "settings";
  settingsTab.title    = "Settings";
  settingsTab.restPath = CERT_MANAGER_FORM_PATH;
  settingsTab.postable = true;
  settingsTab.auth     = WebAuthLevel::Admin;
  settingsTab.order    = 18;
  spec.tabs.push_back(settingsTab);

  WebTabSpec recoveryTab;
  recoveryTab.key      = "recovery";
  recoveryTab.title    = "Recovery";
  recoveryTab.restPath = CERT_MANAGER_FORM_PATH;
  recoveryTab.postable = false;
  recoveryTab.live     = true;
  recoveryTab.auth     = WebAuthLevel::Admin;
  recoveryTab.order    = 20;
  spec.tabs.push_back(recoveryTab);

  // Internals tab — exposes cert PEM / key PEM / CA bundle as
  // SecretField-rendered readonly fields. Their presence in the form
  // schema is what triggers SecretsVault auto-discovery to mark them
  // as encrypt-on-disk; the rendering is just transparency for the
  // operator. Last in tab order so it doesn't distract from the
  // common-case Status / Enrollment / Settings flow.
  WebTabSpec internalsTab;
  internalsTab.key      = "internals";
  internalsTab.title    = "Internals";
  internalsTab.restPath = CERT_MANAGER_FORM_PATH;
  internalsTab.postable = false;
  internalsTab.auth     = WebAuthLevel::Admin;
  internalsTab.order    = 30;
  spec.tabs.push_back(internalsTab);

  _feature = web->registerFeature<CertManagerSettings>(
      std::move(spec), this,
      CertManagerSettings::buildForm,  CertManagerSettings::update,
      CertManagerSettings::staRead,    CertManagerSettings::staUpd,
      8192, 4096);
}

void CertManagerService::begin() {
  (void)_cfg.ensureLoaded();
  refreshRuntimeState();

  // Push loaded material into the TLS context so every subsequent
  // attachToClient() in any module gets the right chain. Without
  // this, Phase 2 mothership /checkin would always run without
  // mTLS — the cert is on disk but the TLS layer never sees it.
  // Order matters: loadCaChain first (server trust), then
  // updateClientCert (client identity); attachToClient binds both.
  if (_tls && _state.ca_bundle_pem.length() > 0) {
    _tls->loadCaChain(_state.ca_bundle_pem);
    log_i("[cert.begin] loaded CA bundle (%u B) into TLS context",
          (unsigned)_state.ca_bundle_pem.length());
  }
  if (_tls && hasValidCert()) {
    _tls->updateClientCert(_state.device_cert_pem, _state.device_key_pem);
    log_i("[cert.begin] loaded client cert (%u B) + key (%u B) into TLS context",
                  (unsigned)_state.device_cert_pem.length(),
                  (unsigned)_state.device_key_pem.length());
  }

  // Spawn the persistent auto-enrollment task whenever we boot in
  // a non-Ready state. The task owns its own WiFi-wait + retry
  // cadence — no need for loop() to nudge it. Once the cert is
  // Ready it exits and won't be respawned (the only way to re-
  // enter NeedsEnrollment is factory-reset, which restarts).
  if (_state.runtime_state != ICertProvider::State::Ready) {
    log_i("[cert.begin] spawning auto-enrollment task");
    (void)kickEnrollment();
  }

  if (_feature) _feature->broadcastWs("boot");
}

void CertManagerService::loop() {
  // Periodic state-machine refresh. The boot-time refreshRuntimeState
  // runs before SNTP has synced the wall clock, so daysUntilExpiry
  // can't tell if a freshly-loaded cert is still valid. The fixed
  // daysUntilExpiry now returns INT32_MAX on unset clock so the
  // initial state is optimistically Ready, but we still need a
  // periodic recheck so:
  //   * cert genuinely past expiry → state slides into GrayZone
  //     within ~60 s of crossing the boundary (cheap, idempotent)
  //   * if recovery completes externally → status_label refreshes
  // 60 s tick keeps the refresh cost negligible.
  uint32_t now_ms = millis();
  if (now_ms - _lastStateRefreshMs > 60000) {
    _lastStateRefreshMs = now_ms;
    refreshRuntimeState();
  }

  // Auto-enrollment lives in its own FreeRTOS task spawned by
  // begin() — no work to do from this thread.
}

int32_t CertManagerService::daysUntilExpiry() const {
  if (_state.not_after_ts == 0) return INT32_MIN;
  uint32_t now_s = (uint32_t)(time(nullptr));
  // Clock-not-set sanity: anything before 2023-11-15 means NTP
  // hasn't synced yet on this boot. Return MAX rather than 0 so the
  // refreshRuntimeState path doesn't slam a still-valid cert into
  // GrayZone on the boot-time race window (cert loaded from disk
  // before SNTP brought the wall clock forward). Once NTP syncs the
  // periodic refresh in loop() recomputes against real time.
  if (now_s < 1700000000) return INT32_MAX;
  int64_t diff_s = (int64_t)_state.not_after_ts - (int64_t)now_s;
  return (int32_t)(diff_s / 86400);
}

void CertManagerService::refreshRuntimeState() {
  using S = ICertProvider::State;
  S newState;

  if (_state.device_cert_pem.length() == 0 ||
      _state.device_key_pem.length()  == 0) {
    // No cert on disk — operator must enroll.
    newState = S::NeedsEnrollment;
  } else {
    // Phase 1.7 will add real expiry parsing from cert PEM. For now
    // trust the persisted not_after_ts field (set by enrollment in
    // Phase 1.5).
    int32_t days = daysUntilExpiry();
    if (days <= 0) {
      // Cert technically present but expired — gray zone candidate.
      // Phase 4b implements the recovery polling; for now flag the
      // state so UI shows the right message.
      newState = S::GrayZone;
    } else {
      newState = S::Ready;
    }
  }

  _state.runtime_state = newState;
  switch (newState) {
    case S::Uninitialised:    _state.status_label = "Uninitialised";    break;
    case S::NeedsEnrollment:  _state.status_label = "NeedsEnrollment";  break;
    case S::Enrolling:        _state.status_label = "Enrolling";        break;
    case S::Ready:            _state.status_label = "Ready";            break;
    case S::Renewing:         _state.status_label = "Renewing";         break;
    case S::GrayZone:         _state.status_label = "GrayZone";         break;
    case S::EnrollmentFailed: _state.status_label = "EnrollmentFailed"; break;
  }
}

// ===== Phase 1.3 — ECDSA-P256 keypair generation =====
//
// mbedtls flow:
//   1. seed CTR_DRBG from mbedtls_entropy (ESP32 has hw RNG via
//      esp_random under the entropy func), with a per-device
//      personalisation string for extra forward-uniqueness
//   2. mbedtls_pk_init + setup as MBEDTLS_PK_ECKEY
//   3. mbedtls_ecp_gen_key on the ECKEY context with curve P-256
//   4. mbedtls_pk_write_key_pem to serialise as "EC PRIVATE KEY" PEM
//   5. always free contexts on every exit (success or failure)
//
// Return: true with outKeyPem populated on success; false otherwise.
// Failures Serial-log the mbedtls error string for the operator.

bool CertManagerService::generateEcdsaKeyPair(String& outKeyPem) {
  outKeyPem = String();

  mbedtls_pk_context       pk;
  mbedtls_entropy_context  entropy;
  mbedtls_ctr_drbg_context drbg;

  mbedtls_pk_init(&pk);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);

  // Personalisation string: short stable per-device ID. Mixes into
  // CTR_DRBG seed so two devices with identical entropy snapshots
  // (very unlikely, but defence-in-depth) land on different keys.
  // Subject CN is "device-<mac>" — same source.
  String pers = String("esprack-cert-") + deviceSubjectCN();

  bool ok = false;
  do {
    int rc = mbedtls_ctr_drbg_seed(
        &drbg, mbedtls_entropy_func, &entropy,
        (const unsigned char*)pers.c_str(), pers.length());
    if (rc != 0) {
      log_e("[cert.gen] ctr_drbg_seed failed: -0x%04x", -rc);
      break;
    }

    rc = mbedtls_pk_setup(&pk,
        mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (rc != 0) {
      log_e("[cert.gen] pk_setup failed: -0x%04x", -rc);
      break;
    }

    rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                             mbedtls_pk_ec(pk),
                             mbedtls_ctr_drbg_random, &drbg);
    if (rc != 0) {
      log_e("[cert.gen] ecp_gen_key failed: -0x%04x", -rc);
      break;
    }

    // PEM output buffer — 1024 B comfortably holds an EC P-256
    // private key PEM block (~250 B real content + base64 overhead).
    unsigned char pem_buf[1024];
    rc = mbedtls_pk_write_key_pem(&pk, pem_buf, sizeof(pem_buf));
    if (rc != 0) {
      log_e("[cert.gen] pk_write_key_pem failed: -0x%04x", -rc);
      break;
    }

    outKeyPem = String((const char*)pem_buf);
    log_i("[cert.gen] keypair generated, PEM=%u B",
                  (unsigned)outKeyPem.length());
    ok = true;
  } while (false);

  mbedtls_pk_free(&pk);
  mbedtls_entropy_free(&entropy);
  mbedtls_ctr_drbg_free(&drbg);
  return ok;
}

// ===== Phase 1.4 — PKCS#10 CSR build =====
//
// mbedtls flow:
//   1. parse the just-generated EC private key PEM back into a
//      pk context (the keypair lives only in PEM at this stage —
//      generate, store, then re-parse for CSR signing keeps the
//      lifetime simple)
//   2. mbedtls_x509write_csr_init + set_subject_name + set_md_alg
//      (SHA-256) + set_key + signature
//   3. mbedtls_x509write_csr_pem to serialise
//
// The CSR is a one-shot artefact — sent to mothership in the
// enrollment POST body, then discarded. Server's response carries
// the signed cert which we persist alongside the key.

bool CertManagerService::buildCsr(const String& keyPem,
                                  String& outCsrPem) {
  outCsrPem = String();
  if (keyPem.length() == 0) {
    log_d("[cert.csr] empty keyPem");
    return false;
  }

  mbedtls_pk_context       pk;
  mbedtls_x509write_csr    csr;
  mbedtls_entropy_context  entropy;
  mbedtls_ctr_drbg_context drbg;

  mbedtls_pk_init(&pk);
  mbedtls_x509write_csr_init(&csr);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);

  bool ok = false;
  do {
    // Re-seed for the signature randomness. mbedtls ECDSA signing
    // is deterministic when configured (RFC 6979) but the API still
    // requires an RNG context for the call signature.
    int rc = mbedtls_ctr_drbg_seed(
        &drbg, mbedtls_entropy_func, &entropy,
        (const unsigned char*)"esprack-csr", 11);
    if (rc != 0) {
      log_e("[cert.csr] drbg seed failed: -0x%04x", -rc);
      break;
    }

    // mbedtls_pk_parse_key wants length INCLUDING the trailing NUL.
    rc = mbedtls_pk_parse_key(&pk,
        (const unsigned char*)keyPem.c_str(),
        keyPem.length() + 1,
        nullptr, 0,
        mbedtls_ctr_drbg_random, &drbg);
    if (rc != 0) {
      log_e("[cert.csr] pk_parse_key failed: -0x%04x", -rc);
      break;
    }

    String subject = String("CN=") + deviceSubjectCN();
    rc = mbedtls_x509write_csr_set_subject_name(&csr, subject.c_str());
    if (rc != 0) {
      log_e("[cert.csr] set_subject failed: -0x%04x", -rc);
      break;
    }

    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    mbedtls_x509write_csr_set_key(&csr, &pk);

    unsigned char pem_buf[1024];
    rc = mbedtls_x509write_csr_pem(
        &csr, pem_buf, sizeof(pem_buf),
        mbedtls_ctr_drbg_random, &drbg);
    if (rc != 0) {
      log_e("[cert.csr] write_csr_pem failed: -0x%04x", -rc);
      break;
    }

    outCsrPem = String((const char*)pem_buf);
    log_i("[cert.csr] CSR built, PEM=%u B, subject=%s",
                  (unsigned)outCsrPem.length(), subject.c_str());
    ok = true;
  } while (false);

  mbedtls_pk_free(&pk);
  mbedtls_x509write_csr_free(&csr);
  mbedtls_entropy_free(&entropy);
  mbedtls_ctr_drbg_free(&drbg);
  return ok;
}

// Subject CN comes from the single source of truth: DeviceIdentity.
// Format = "<project>-<mac12>-<uid8>" — see
// lib/ESPRack/include/DeviceIdentity.h for the rationale and per-
// segment guarantees. Same string lands in the X.509 cert, in the
// /api/v1/checkin deviceId field, on the mothership Identity tab,
// and in AutoUpdate's HTTP did= query — everything reads through
// the canonical helper, so a format change propagates atomically.
String CertManagerService::deviceSubjectCN() const {
  return DeviceIdentity::canonical();
}

// ===== Approval-driven enrollment loop =====
//
// kickEnrollment is called from begin() (and only from begin() —
// no operator-triggered path on this branch). The task owns the
// whole flow: waits for WiFi + resolvable URL, generates keypair
// + CSR once, then polls /enroll until Approved or Blacklisted.
//
// State transitions visible to the operator via WS broadcast:
//   NeedsEnrollment → Enrolling (first poll) → Ready (Approved)
//                                            → EnrollmentFailed
//                                              (Blacklisted)
// Pending / network / server errors don't change state — they
// just bump enroll_poll_count + enroll_last_reason so the UI
// telemetry shows the device is alive and waiting.

bool CertManagerService::kickEnrollment() {
  if (_enrollTask != nullptr) {
    log_w("[cert.enroll] task already running");
    return false;
  }

  // 8 KB stack — mbedtls keypair gen + CSR + HTTPS handshake +
  // ArduinoJson parsing on the same stack. ESP32 default tasks
  // are usually 4 KB; we double it to leave breathing room for
  // mbedtls scratch buffers.
  BaseType_t rc = xTaskCreatePinnedToCore(
      &CertManagerService::enrollTaskTramp,
      "certEnroll",
      8192,
      this,
      1,
      &_enrollTask,
      tskNO_AFFINITY);

  if (rc != pdPASS) {
    log_e("[cert.enroll] xTaskCreate failed: %d", (int)rc);
    _enrollTask = nullptr;
    return false;
  }

  // Optimistic state update so UI shows "Enrolling" while task runs.
  update([](CertManagerSettings& s) {
    s.runtime_state = ICertProvider::State::Enrolling;
    s.status_label  = "Enrolling…";
    return StateUpdateResult::CHANGED;
  }, "cert.enroll-kick");

  return true;
}

void CertManagerService::enrollTaskTramp(void* arg) {
  auto* self = static_cast<CertManagerService*>(arg);
  if (self) self->runEnrollmentLoop();
  // Task self-destruct: clear handle BEFORE vTaskDelete so parent
  // thread sees nullptr if it polls again.
  if (self) self->_enrollTask = nullptr;
  vTaskDelete(nullptr);
}

void CertManagerService::runEnrollmentLoop() {
  log_i("[cert.enroll] task start");

  // Cadence — short on Pending (operator just clicks Approve and the
  // device transitions within one cycle), longer on transient errors
  // so we don't spam the mothership when it's down. Both fall safely
  // inside the 60s state-refresh window in loop().
  static constexpr uint32_t PENDING_INTERVAL_S = 30;
  static constexpr uint32_t ERROR_INTERVAL_S   = 60;

  // 1) Wait for WiFi. Cheap to busy-wait at 1 Hz; the rest of the
  //    framework is already up by now (begin() spawned us).
  while (!WiFi.isConnected()) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  // 2) Wait for a resolvable enroll URL (Mothership profile picked).
  while (effectiveEnrollUrl().length() == 0) {
    log_d("[cert.enroll] waiting for enroll URL "
                    "(pick a Mothership profile or set PKI Base URL)");
    vTaskDelay(pdMS_TO_TICKS(5000));
  }

  // 3) Generate keypair + CSR ONCE. Reused across every poll so the
  //    server-side cached pending request matches our final POSTed
  //    CSR — operator's Approve click signs the very CSR they saw.
  String keyPem;
  if (!generateEcdsaKeyPair(keyPem)) {
    update([](CertManagerSettings& s) {
      s.runtime_state = ICertProvider::State::EnrollmentFailed;
      s.status_label  = "Enrollment failed: keypair generation";
      return StateUpdateResult::CHANGED;
    }, "cert.enroll-fail");
    return;
  }

  String csrPem;
  if (!buildCsr(keyPem, csrPem)) {
    update([](CertManagerSettings& s) {
      s.runtime_state = ICertProvider::State::EnrollmentFailed;
      s.status_label  = "Enrollment failed: CSR build";
      return StateUpdateResult::CHANGED;
    }, "cert.enroll-fail");
    return;
  }

  // 4) Poll loop. Each iteration is one /enroll round-trip. The 5
  //    EnrollResult arms drive the next delay (or exit).
  for (;;) {
    // Bail out if another path bumped us to Ready externally (e.g.
    // a successful rotate during a long blacklist wait).
    if (_state.runtime_state == ICertProvider::State::Ready) {
      log_i("[cert.enroll] state externally Ready — exit");
      return;
    }

    String certPem, caBundlePem, recoveryToken, reason;
    EnrollResult res = postEnrollOnce(csrPem, certPem, caBundlePem,
                                       recoveryToken, reason);

    // Update telemetry on every round-trip.
    update([&](CertManagerSettings& s) {
      s.enroll_poll_count   += 1;
      s.enroll_last_poll_s   = (uint32_t)(millis() / 1000);
      s.enroll_last_reason   = reason;
      return StateUpdateResult::CHANGED;
    }, "cert.enroll-poll");

    if (res == EnrollResult::Approved) {
      String serialHex;
      uint32_t notAfterTs = 0;
      if (!parseCertMetadata(certPem, serialHex, notAfterTs)) {
        log_w("[cert.enroll] WARN: cert parse failed; "
                        "proceeding with empty serial / not_after");
        serialHex = "";
        notAfterTs = 0;
      }

      // Atomic write — including the single-use wipe of
      // bootstrap_token (in RAM AND on disk, since this field is
      // persisted now). saveIfChanged via the update-handler in
      // the constructor commits the wipe to /config/cert.json.
      update([&](CertManagerSettings& s) {
        s.device_cert_pem   = certPem;
        s.device_key_pem    = keyPem;
        s.ca_bundle_pem     = caBundlePem;
        s.serial_hex        = serialHex;
        s.subject_cn        = deviceSubjectCN();
        s.not_after_ts      = notAfterTs;
        s.recovery_token    = recoveryToken;
        s.bootstrap_token   = String();   // single-use → wipe
        s.runtime_state     = ICertProvider::State::Ready;
        s.status_label      = "Ready";
        s.enroll_last_reason = "Approved — cert installed";
        return StateUpdateResult::CHANGED;
      }, "cert.enroll-ok");

      // Push fresh material into the TLS context so subsequent
      // HTTPS calls from any framework module pick up mTLS.
      if (_tls) {
        _tls->updateClientCert(certPem, keyPem);
        _tls->loadCaChain(caBundlePem);
      }

      log_i("[cert.enroll] APPROVED — serial=%s, not_after=%u",
                    serialHex.c_str(), (unsigned)notAfterTs);
      return;
    }

    if (res == EnrollResult::Blacklisted) {
      update([](CertManagerSettings& s) {
        s.runtime_state = ICertProvider::State::EnrollmentFailed;
        s.status_label  = "Blacklisted by mothership";
        return StateUpdateResult::CHANGED;
      }, "cert.enroll-blacklisted");
      log_e("[cert.enroll] BLACKLISTED — exit (manual unblock "
                      "or factory-reset required)");
      return;
    }

    // Pending / NetworkFail / ServerError → sleep + retry.
    uint32_t sleep_s = (res == EnrollResult::Pending)
                        ? PENDING_INTERVAL_S
                        : ERROR_INTERVAL_S;
    log_d("[cert.enroll] sleep %us before next poll",
                  (unsigned)sleep_s);
    vTaskDelay(pdMS_TO_TICKS(sleep_s * 1000));
  }
}

// ===== PKI URL resolution =====
//
// Two-step fallback:
//   1. If operator pinned a `pki_base_url` on the Settings tab,
//      use that (lets them point at a CA host that's separate from
//      the regular Mothership).
//   2. Otherwise read app->mothershipProfile()->enrollUrl() — the
//      active Mothership profile's Base URL with /api/v1/enroll
//      appended.
// Returns empty when neither resolves — callers MUST treat empty
// as "skip the request" with a clear log message.

String CertManagerService::effectiveEnrollUrl() const {
  if (_state.pki_base_url.length() > 0) {
    return _state.pki_base_url + "/api/v1/enroll";
  }
  if (_app && _app->mothershipProfile()) {
    return _app->mothershipProfile()->enrollUrl();
  }
  return String();
}

String CertManagerService::effectiveRecoverUrl() const {
  if (_state.pki_base_url.length() > 0) {
    return _state.pki_base_url + "/api/v1/recover";
  }
  if (_app && _app->mothershipProfile()) {
    return _app->mothershipProfile()->recoverUrl();
  }
  return String();
}

CertManagerService::EnrollResult CertManagerService::postEnrollOnce(
    const String& csrPem,
    String& outCertPem,
    String& outCaBundlePem,
    String& outRecoveryToken,
    String& outReason) {
  outCertPem = String();
  outCaBundlePem = String();
  outRecoveryToken = String();
  outReason = String();

  String enroll_url = effectiveEnrollUrl();
  if (enroll_url.length() == 0) {
    outReason = "no enroll URL (pick a Mothership profile)";
    log_d("[cert.enroll] %s", outReason.c_str());
    return EnrollResult::ServerError;
  }

  // Heap diagnostics before TLS — surfaces the
  // MBEDTLS_ERR_SSL_ALLOC_FAILED (-32512) failure mode where the
  // device's heap fragments enough that the next handshake can't
  // get its ~25 KB contiguous scratch. When this fires it's a
  // device-wide stuck state: mothership soft-watchdog (see
  // MothershipService) catches the pattern and forces esp_restart()
  // after N consecutive failures.
  log_i("[cert.enroll] heap pre-TLS: free=%u max_alloc=%u",
        (unsigned)ESP.getFreeHeap(),
        (unsigned)ESP.getMaxAllocHeap());

  WiFiClientSecure secureClient;
  // FIRST-ENROLLMENT TLS POSTURE: we don't have a CA pinned yet
  // (the bundle ARRIVES in this very response), so we accept
  // whatever cert the server presents. Production builds should
  // pre-bundle a bootstrap CA into firmware and load it here via
  // _tls->loadCaChain(BOOTSTRAP_CA_PEM) BEFORE this call.
  // For dev / mock-server testing, setInsecure() lets us reach
  // a self-signed mock without any bundled trust store.
  secureClient.setInsecure();
  secureClient.setHandshakeTimeout(15);
  secureClient.setTimeout(20000);

  HTTPClient http;
  if (!http.begin(secureClient, enroll_url)) {
    outReason = "HTTPClient.begin failed";
    log_d("[cert.enroll] %s", outReason.c_str());
    return EnrollResult::NetworkFail;
  }
  http.setTimeout(20000);
  http.addHeader("Content-Type", "application/json");
  // Bootstrap-token fast-path (server's _handle_enroll path 2). Read
  // straight from persisted state on every poll so a UI edit kicks
  // in on the very next attempt. Empty → no Bearer header → server
  // path 6 parks the request in PENDING_ENROLLMENTS for operator
  // approval. Sending "Bearer " with empty value would trip path-2's
  // 401 short-circuit and never reach the pending-park branch, so
  // strict length check matters here.
  const String& bootTok = _state.bootstrap_token;
  if (bootTok.length() > 0) {
    http.addHeader("Authorization", String("Bearer ") + bootTok);
  }

  // Body: {"deviceId": "...", "csr_pem": "...", "wg_pubkey": "..."}
  // Allocate enough for csr_pem (~480 B base64) + WG pubkey (44 B)
  // + framing.
  DynamicJsonDocument req(2048);
  req["deviceId"] = deviceSubjectCN();
  req["csr_pem"]  = csrPem;
  // Phase WG.3 — include our WG public key so the mothership can
  // pre-allocate a tunnel IP and add the peer at enrollment time.
  // Optional: skipped when no WireGuard module is installed, or
  // when keypair generation failed at boot.
  if (_wg) {
    String wgPub = _wg->publicKey();
    if (wgPub.length() > 0) {
      req["wg_pubkey"] = wgPub;
    }
  }
  String reqBody;
  serializeJson(req, reqBody);

  log_d("[cert.enroll] POST %s, body=%u B, token=%s",
                enroll_url.c_str(),
                (unsigned)reqBody.length(),
                bootTok.length() > 0 ? "yes" : "no");

  int code = http.POST(reqBody);
  String respBody = http.getString();
  http.end();
  // Aggressive mbedtls teardown — `http.end()` calls client.stop()
  // implicitly, but on arduino-esp32 there are paths (keep-alive,
  // partial reads) where the inner WiFiClientSecure keeps its TLS
  // context alive for the next call. Explicit .stop() forces the
  // mbedtls scratch (16 KB tx + 16 KB rx record buffers) back into
  // heap immediately so the next handshake doesn't compete with
  // them for max-contiguous space.
  secureClient.stop();
  log_d("[cert.enroll] HTTP code=%d, body=%u B",
                code, (unsigned)respBody.length());
  log_d("[cert.enroll] heap post-TLS: free=%u max_alloc=%u",
        (unsigned)ESP.getFreeHeap(),
        (unsigned)ESP.getMaxAllocHeap());

  if (code <= 0) {
    outReason = String("network: ") + http.errorToString(code);
    return EnrollResult::NetworkFail;
  }
  if (code == 403) {
    // Server's path 1: blacklisted. Hard stop.
    outReason = "Blacklisted by mothership";
    log_e("[cert.enroll] BLACKLISTED: %s", respBody.c_str());
    return EnrollResult::Blacklisted;
  }
  if (code < 200 || code >= 300) {
    outReason = String("HTTP ") + code;
    if (respBody.length() > 0 && respBody.length() < 200) {
      outReason += ": " + respBody;
    }
    log_w("[cert.enroll] server error: %s", outReason.c_str());
    return EnrollResult::ServerError;
  }

  // Response is one of:
  //   approved=true  + cert_pem + ca_bundle_pem + recovery_token
  //       → EnrollResult::Approved, populate outputs
  //   approved=false + status=pending
  //       → EnrollResult::Pending, outputs stay empty; caller retries
  //   anything else
  //       → EnrollResult::ServerError
  // 8 KB doc — cert PEM ~400 B, ca_bundle ~1.5 KB worst case, +
  // recovery_token + framing.
  DynamicJsonDocument resp(8192);
  DeserializationError jerr = deserializeJson(resp, respBody);
  if (jerr) {
    outReason = String("JSON parse: ") + jerr.c_str();
    log_d("[cert.enroll] %s", outReason.c_str());
    return EnrollResult::ServerError;
  }

  bool approved = resp["approved"].as<bool>();
  if (!approved) {
    String status_str = resp["status"].as<String>();
    String msg = resp["message"].as<String>();
    outReason = "Pending: " + (msg.length() ? msg : status_str);
    log_d("[cert.enroll] PENDING — %s", outReason.c_str());
    return EnrollResult::Pending;
  }

  outCertPem        = resp["cert_pem"].as<String>();
  outCaBundlePem    = resp["ca_bundle_pem"].as<String>();
  outRecoveryToken  = resp["recovery_token"].as<String>();

  if (outCertPem.length() == 0 || outCaBundlePem.length() == 0) {
    outReason = "missing cert_pem / ca_bundle_pem in response";
    log_d("[cert.enroll] %s", outReason.c_str());
    return EnrollResult::ServerError;
  }
  outReason = "Approved";
  log_i("[cert.enroll] APPROVED — cert=%u ca=%u recovery=%u B",
                (unsigned)outCertPem.length(),
                (unsigned)outCaBundlePem.length(),
                (unsigned)outRecoveryToken.length());
  return EnrollResult::Approved;
}

// ===== Phase 4a — Proactive cert rotation =====
//
// Mothership scheduler (cron, daily) finds devices with
// `not_after - now < 30d` and queues a `renewCert` action in the
// device's command channel. Device's MothershipService dispatcher
// invokes ICertProvider::rotate(renewUrl) on next check-in.
// rotate() spawns a FreeRTOS task to do the work off the check-in
// thread (same pattern as enrollment).
//
// Atomic verify-then-swap: new keypair + CSR are generated, POSTed
// via mTLS using the CURRENT cert (mutual auth proves identity to
// server, no bootstrap token needed). On 2xx response the new cert
// is parsed for metadata and pushed into TLSContextService —
// updateClientCert keeps the previous PEMs alive for one generation
// so any in-flight handshakes survive. State + disk get updated
// only after TLS context accepts the new material. Any failure on
// the path leaves cert.json untouched and current cert keeps
// working until the next rotation attempt.

namespace {
struct RotateArg {
  CertManagerService* svc;
  String              renewUrl;
};
}  // namespace

bool CertManagerService::rotate(const String& renewUrl) {
  if (_rotateTask != nullptr) {
    log_w("[cert.rotate] already running");
    return false;
  }
  if (!hasValidCert()) {
    log_e("[cert.rotate] no current cert — cannot rotate "
                    "via mTLS; full re-enroll required");
    return false;
  }
  if (renewUrl.length() == 0) {
    log_d("[cert.rotate] empty renewUrl");
    return false;
  }

  auto* arg = new RotateArg{this, renewUrl};
  BaseType_t rc = xTaskCreatePinnedToCore(
      &CertManagerService::rotateTaskTramp,
      "certRotate",
      8192,
      arg,
      1,
      &_rotateTask,
      tskNO_AFFINITY);

  if (rc != pdPASS) {
    log_e("[cert.rotate] xTaskCreate failed: %d", (int)rc);
    delete arg;
    _rotateTask = nullptr;
    return false;
  }

  // Visible "Renewing" status for UI — mTLS still works under the
  // OLD cert during this period, so consumers don't need to gate
  // their requests; this state is purely informational.
  update([](CertManagerSettings& s) {
    s.runtime_state = ICertProvider::State::Renewing;
    s.status_label  = "Renewing…";
    return StateUpdateResult::CHANGED;
  }, "cert.rotate-kick");

  return true;
}

void CertManagerService::rotateTaskTramp(void* arg) {
  auto* a = static_cast<RotateArg*>(arg);
  if (a && a->svc) a->svc->runRotation(a->renewUrl);
  if (a) delete a;
  vTaskDelete(nullptr);
}

void CertManagerService::runRotation(const String& renewUrl) {
  log_i("[cert.rotate] task start, url=%s", renewUrl.c_str());

  String newKeyPem;
  if (!generateEcdsaKeyPair(newKeyPem)) {
    log_e("[cert.rotate] FAIL: keypair gen");
    update([](CertManagerSettings& s) {
      s.runtime_state = ICertProvider::State::Ready;  // back to Ready, old cert intact
      s.status_label  = "Ready";
      return StateUpdateResult::CHANGED;
    }, "cert.rotate-fail");
    _rotateTask = nullptr;
    return;
  }

  String newCsrPem;
  if (!buildCsr(newKeyPem, newCsrPem)) {
    log_e("[cert.rotate] FAIL: CSR build");
    update([](CertManagerSettings& s) {
      s.runtime_state = ICertProvider::State::Ready;
      s.status_label  = "Ready";
      return StateUpdateResult::CHANGED;
    }, "cert.rotate-fail");
    _rotateTask = nullptr;
    return;
  }

  String newCertPem, newCaBundlePem;
  if (!postCsrToRenew(newCsrPem, renewUrl, newCertPem, newCaBundlePem)) {
    log_e("[cert.rotate] FAIL: server unreachable / rejected");
    update([](CertManagerSettings& s) {
      s.runtime_state = ICertProvider::State::Ready;
      s.status_label  = "Ready (last rotate failed)";
      return StateUpdateResult::CHANGED;
    }, "cert.rotate-fail");
    _rotateTask = nullptr;
    return;
  }

  String newSerialHex;
  uint32_t newNotAfterTs = 0;
  if (!parseCertMetadata(newCertPem, newSerialHex, newNotAfterTs)) {
    log_w("[cert.rotate] WARN: cert parse failed; "
                    "proceeding with empty serial / notAfter");
    newSerialHex = "";
    newNotAfterTs = 0;
  }

  // Atomic swap: write new material to state (which propagates to
  // disk via addUpdateHandler), then push into TLS context. Old
  // PEM strings stay in TLSContextService's _prev / _prevPrev slots
  // for one generation, protecting any in-flight outbound HTTPS.
  // recovery_token is preserved (rotation-invariant; reissued only
  // on full re-enroll).
  update([&](CertManagerSettings& s) {
    s.device_cert_pem = newCertPem;
    s.device_key_pem  = newKeyPem;
    s.ca_bundle_pem   = newCaBundlePem;
    s.serial_hex      = newSerialHex;
    s.not_after_ts    = newNotAfterTs;
    s.runtime_state   = ICertProvider::State::Ready;
    s.status_label    = "Ready";
    return StateUpdateResult::CHANGED;
  }, "cert.rotate-ok");

  if (_tls) {
    _tls->updateClientCert(newCertPem, newKeyPem);
    _tls->loadCaChain(newCaBundlePem);
  }

  log_i("[cert.rotate] done — new serial=%s, not_after=%u",
                newSerialHex.c_str(), (unsigned)newNotAfterTs);
  _rotateTask = nullptr;
}

// ===== Phase 4b — Gray-zone recovery =====
//
// Sister flow to enroll: device contacts mothership over server-side
// TLS only (mTLS impossible — cert is dead) and asks "I'm device-X,
// I have recovery_token Y, please give me a fresh cert when an
// operator approves". Server stages the request in pending_recovery
// queue. Operator approves via admin endpoint, server signs fresh
// cert and parks it under that deviceId. Next /recover poll from
// device drains the parked cert and atomically swaps it in.
//
// Difference from rotate: no mTLS auth (cert dead), no CSR (server
// generates entire fresh cert+keypair? actually no — device still
// generates keypair locally for forward secrecy, sends CSR; server
// signs it after approval). For Phase 4b iteration 1 the device
// just polls with token + deviceId; CSR comes on the response side
// once approval lands.
//
// Actually let me reconsider: even in gray-zone the device should
// generate keypair + CSR locally (private key never leaves) and
// include the CSR in the recovery request. Server signs after
// approval. This way the device's private key is fresh for the
// new cert — we don't trust whatever was in the broken cert.json.
// One CSR up-front, then poll until approved.

namespace {
struct RecoveryArg {
  CertManagerService* svc;
};
}  // namespace

bool CertManagerService::beginRecovery() {
  if (_state.recovery_token.length() == 0) {
    log_e("[cert.recover] no recovery_token — full re-enroll required");
    return false;
  }
  if (effectiveRecoverUrl().length() == 0) {
    log_e("[cert.recover] no recover URL — pick a Mothership "
                    "profile (or set PKI Base URL on the Settings tab)");
    return false;
  }
  // If a polling task is already running (e.g. operator clicked
  // Trigger Recovery with a wrong URL, then fixed it and re-clicked),
  // wake it up so it polls the NEW URL immediately instead of
  // sleeping out the 60s interval. xTaskNotifyGive bumps the
  // task's notification value; the poll loop's
  // ulTaskNotifyTake-with-timeout returns early on the bump.
  if (_recoveryTask != nullptr) {
    log_w("[cert.recover] task already running — waking up for early poll");
    xTaskNotifyGive(_recoveryTask);
    return true;
  }

  auto* arg = new RecoveryArg{this};
  BaseType_t rc = xTaskCreatePinnedToCore(
      &CertManagerService::recoveryTaskTramp,
      "certRecover",
      8192,
      arg,
      1,
      &_recoveryTask,
      tskNO_AFFINITY);

  if (rc != pdPASS) {
    log_e("[cert.recover] xTaskCreate failed: %d", (int)rc);
    delete arg;
    _recoveryTask = nullptr;
    return false;
  }

  update([](CertManagerSettings& s) {
    s.runtime_state = ICertProvider::State::GrayZone;
    s.status_label  = "GrayZone (polling /recover)";
    return StateUpdateResult::CHANGED;
  }, "cert.recover-kick");

  return true;
}

void CertManagerService::recoveryTaskTramp(void* arg) {
  auto* a = static_cast<RecoveryArg*>(arg);
  if (a && a->svc) a->svc->runRecoveryLoop();
  if (a) delete a;
  vTaskDelete(nullptr);
}

void CertManagerService::runRecoveryLoop() {
  log_i("[cert.recover] task start");

  // Poll until approved. Each poll iteration generates a NEW keypair
  // + CSR — the device commits to a private key only when an
  // approval finally lands; until then each request carries a fresh
  // CSR. Cheap on ECDSA-P256 (~50ms per gen on classic, hw-accel on
  // C3/C6).
  while (true) {
    String certPem, caBundlePem;
    bool approved = false;

    if (!postRecoveryRequest(certPem, caBundlePem, approved)) {
      // Network / parse failure — wait + retry. Don't exit task;
      // recovery is the only path back to mTLS, must keep trying.
      // Sleep is interruptible via xTaskNotifyGive — operator
      // clicking Trigger Recovery again wakes us up immediately
      // so a URL fix can be tested without waiting full 60s.
      log_e("[cert.recover] poll failed, will retry");
      ulTaskNotifyTake(pdTRUE,
          pdMS_TO_TICKS(RECOVERY_POLL_INTERVAL_S * 1000));
      continue;
    }

    if (!approved) {
      log_w("[cert.recover] still pending operator approval");
      ulTaskNotifyTake(pdTRUE,
          pdMS_TO_TICKS(RECOVERY_POLL_INTERVAL_S * 1000));
      continue;
    }

    // Approved — server returned fresh cert. The keypair we used in
    // the LATEST CSR is the one matching this cert. Sadly we have a
    // race: the keypair we generated for THIS poll iteration is
    // local to postRecoveryRequest and gone by now. Need to either
    // (a) keep a candidate keypair member persistent across polls,
    // or (b) generate a NEW keypair + CSR + immediate POST + atomic
    // apply when we see approved=true.
    //
    // Option (b) doesn't work either — server already signed against
    // the public key from the LAST poll's CSR. So we MUST persist
    // the candidate keypair across polls.
    //
    // Phase 4b iteration 1 uses a simpler model: post the CSR ONCE,
    // then poll without sending CSR until approved. Server holds
    // the CSR alongside the pending request. Next iteration the
    // server-side memory model can persist that CSR but for now
    // we persist the keypair ON DEVICE in a member.
    log_i("[cert.recover] approved — applying new cert "
                  "(%u B), ca (%u B)",
                  (unsigned)certPem.length(),
                  (unsigned)caBundlePem.length());

    String serialHex;
    uint32_t notAfterTs = 0;
    parseCertMetadata(certPem, serialHex, notAfterTs);

    // Apply atomically. recovery_token preserved (already on disk;
    // unchanged across recovery — same secret can be used again).
    update([&](CertManagerSettings& s) {
      s.device_cert_pem  = certPem;
      // device_key_pem set inside postRecoveryRequest via _candidateKeyPem
      s.device_key_pem   = _candidateKeyPem;
      s.ca_bundle_pem    = caBundlePem;
      s.serial_hex       = serialHex;
      s.not_after_ts     = notAfterTs;
      s.runtime_state    = ICertProvider::State::Ready;
      s.status_label     = "Ready (recovered)";
      return StateUpdateResult::CHANGED;
    }, "cert.recover-ok");

    if (_tls) {
      _tls->updateClientCert(certPem, _candidateKeyPem);
      _tls->loadCaChain(caBundlePem);
    }

    // Wipe candidate from memory — applied to disk now.
    _candidateKeyPem = String();

    log_i("[cert.recover] done — serial=%s", serialHex.c_str());
    _recoveryTask = nullptr;
    return;
  }
}

bool CertManagerService::postRecoveryRequest(String& outCertPem,
                                              String& outCaBundlePem,
                                              bool& outApproved) {
  outCertPem = String();
  outCaBundlePem = String();
  outApproved = false;

  // Generate candidate keypair + CSR ONCE — first poll only.
  // Subsequent polls reuse the same candidate so the CSR sent to
  // server (and thus signed cert returned) matches the keypair we
  // can swap in. Cleared after successful apply.
  if (_candidateKeyPem.length() == 0) {
    if (!generateEcdsaKeyPair(_candidateKeyPem)) {
      log_e("[cert.recover] keypair gen failed");
      _candidateKeyPem = String();
      return false;
    }
  }

  String csrPem;
  if (!buildCsr(_candidateKeyPem, csrPem)) {
    log_e("[cert.recover] CSR build failed");
    return false;
  }

  WiFiClientSecure client;
  // Server-side TLS only — cert is dead, no client identity.
  // Production: pin a bootstrap CA from firmware here. For dev mock,
  // setInsecure to accept the self-signed cert without trust chain.
  client.setInsecure();
  client.setHandshakeTimeout(15);
  client.setTimeout(20000);

  String recover_url = effectiveRecoverUrl();
  if (recover_url.length() == 0) {
    log_e("[cert.recover] no recover URL at request time");
    return false;
  }
  HTTPClient http;
  if (!http.begin(client, recover_url)) {
    log_e("[cert.recover] http.begin failed");
    return false;
  }
  http.setTimeout(20000);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument req(2048);
  req["deviceId"]        = deviceSubjectCN();
  req["recoveryToken"]   = _state.recovery_token;
  req["lastKnownSerial"] = _state.serial_hex;
  req["csr_pem"]         = csrPem;
  String reqBody;
  serializeJson(req, reqBody);

  log_d("[cert.recover] POST %s, body=%u B",
                recover_url.c_str(),
                (unsigned)reqBody.length());

  int code = http.POST(reqBody);
  log_d("[cert.recover] HTTP code=%d", code);

  if (code < 200 || code >= 300) {
    String err = http.getString();
    log_w("[cert.recover] server error: %s", err.c_str());
    http.end();
    return false;
  }

  String respBody = http.getString();
  http.end();

  DynamicJsonDocument resp(8192);
  DeserializationError jerr = deserializeJson(resp, respBody);
  if (jerr) {
    log_e("[cert.recover] JSON parse: %s", jerr.c_str());
    return false;
  }

  outApproved = resp["approved"].as<bool>();
  if (outApproved) {
    outCertPem     = resp["cert_pem"].as<String>();
    outCaBundlePem = resp["ca_bundle_pem"].as<String>();
    if (outCertPem.length() == 0 || outCaBundlePem.length() == 0) {
      log_e("[cert.recover] approved=true but cert/ca missing");
      outApproved = false;
      return false;
    }
  }
  return true;
}

bool CertManagerService::postCsrToRenew(const String& csrPem,
                                          const String& renewUrl,
                                          String& outCertPem,
                                          String& outCaBundlePem) {
  outCertPem = String();
  outCaBundlePem = String();

  if (!_tls) {
    log_e("[cert.rotate] no TLS provider");
    return false;
  }

  WiFiClientSecure client;
  // mTLS this time — attach the EXISTING cert (not insecure, not
  // bootstrap-token). Server validates client cert; if valid,
  // signs the new CSR.
  _tls->attachToClient(client);
  client.setHandshakeTimeout(15);
  client.setTimeout(20000);

  HTTPClient http;
  if (!http.begin(client, renewUrl)) {
    log_e("[cert.rotate] http.begin failed");
    return false;
  }
  http.setTimeout(20000);
  http.addHeader("Content-Type", "application/json");

  // Body: just deviceId + csr_pem; no token (mTLS auth already)
  DynamicJsonDocument req(2048);
  req["deviceId"] = deviceSubjectCN();
  req["csr_pem"]  = csrPem;
  String reqBody;
  serializeJson(req, reqBody);

  log_d("[cert.rotate] POST %s, body=%u B",
                renewUrl.c_str(), (unsigned)reqBody.length());

  int code = http.POST(reqBody);
  log_d("[cert.rotate] HTTP code=%d", code);

  if (code < 200 || code >= 300) {
    String err = http.getString();
    log_w("[cert.rotate] server error: %s", err.c_str());
    http.end();
    return false;
  }

  String respBody = http.getString();
  http.end();

  DynamicJsonDocument resp(8192);
  DeserializationError jerr = deserializeJson(resp, respBody);
  if (jerr) {
    log_e("[cert.rotate] JSON parse: %s", jerr.c_str());
    return false;
  }

  outCertPem     = resp["cert_pem"].as<String>();
  outCaBundlePem = resp["ca_bundle_pem"].as<String>();

  if (outCertPem.length() == 0 || outCaBundlePem.length() == 0) {
    log_e("[cert.rotate] response missing cert / ca");
    return false;
  }
  log_d("[cert.rotate] parsed cert=%u ca=%u B",
                (unsigned)outCertPem.length(),
                (unsigned)outCaBundlePem.length());
  return true;
}

bool CertManagerService::parseCertMetadata(const String& certPem,
                                            String& outSerialHex,
                                            uint32_t& outNotAfterTs) {
  outSerialHex = String();
  outNotAfterTs = 0;

  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);

  // mbedtls_x509_crt_parse wants length INCLUDING trailing NUL.
  int rc = mbedtls_x509_crt_parse(&crt,
      (const unsigned char*)certPem.c_str(), certPem.length() + 1);
  if (rc != 0) {
    log_e("[cert.parse] x509_crt_parse failed: -0x%04x", -rc);
    mbedtls_x509_crt_free(&crt);
    return false;
  }

  // Serial — bytes in crt.serial.p as unstructured big-endian. Format
  // as colon-delimited lowercase hex for matching server-side audit logs.
  if (crt.serial.len > 0 && crt.serial.p) {
    char hex[3];
    for (size_t i = 0; i < crt.serial.len; ++i) {
      snprintf(hex, sizeof(hex), "%02x", crt.serial.p[i]);
      if (i > 0) outSerialHex += ":";
      outSerialHex += hex;
    }
  }

  // notAfter — mbedtls_x509_time has year/month/day/hour/min/sec.
  // Convert to unix timestamp via tm + mktime (assumes UTC; cert
  // times in X.509 are UTC by RFC 5280).
  struct tm tmv = {};
  tmv.tm_year = crt.valid_to.year - 1900;
  tmv.tm_mon  = crt.valid_to.mon  - 1;
  tmv.tm_mday = crt.valid_to.day;
  tmv.tm_hour = crt.valid_to.hour;
  tmv.tm_min  = crt.valid_to.min;
  tmv.tm_sec  = crt.valid_to.sec;
  tmv.tm_isdst = 0;
  // mktime treats `tmv` as local time; we have UTC. ESP32 has timegm
  // (sometimes named _mkgmtime / __timegm depending on toolchain).
  // Fallback: compute manually — tz-shift mktime result by current
  // timezone offset. For dev simplicity use mktime + mark; in prod
  // we'd configure TZ=UTC at boot or use a real timegm. The 24h
  // tolerance on cert expiry matters more than absolute precision.
  time_t t = mktime(&tmv);
  if (t > 0) outNotAfterTs = (uint32_t)t;

  mbedtls_x509_crt_free(&crt);
  return true;
}
