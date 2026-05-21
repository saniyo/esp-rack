#include <CertManagerService.h>
#include <ITLSProvider.h>
#include <IWireguardProvider.h>
#include <IMothershipProfileProvider.h>
#include <App.h>
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
  root["pki_base_url"]    = s.pki_base_url;
  root["auto_enroll"]     = s.auto_enroll;
  // bootstrap_token intentionally omitted — never persists.
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

  // ── Plain non-sensitive housekeeping ──
  ch |= FormBuilder::updateValue(root, "serial_hex",      s.serial_hex);
  ch |= FormBuilder::updateValue(root, "subject_cn",      s.subject_cn);
  ch |= FormBuilder::updateValue(root, "not_after_ts",    s.not_after_ts);
  ch |= FormBuilder::updateValue(root, "pki_base_url",    s.pki_base_url);
  ch |= FormBuilder::updateValue(root, "auto_enroll",     s.auto_enroll);

  // Bootstrap token IS the only secret field that the form is
  // EXPECTED to mutate (operator types it during enrollment). Plain
  // updateValue here so empty submits genuinely clear it (e.g. the
  // success path explicitly wipes via runEnrollment, but operator
  // could also clear by hand).
  ch |= FormBuilder::updateValue(root, "bootstrap_token", s.bootstrap_token);

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
  // Operator-driven first-time provisioning. Bootstrap token is
  // single-use-time-bound: server admin UI generates it (24h TTL),
  // operator copies → pastes here → Enroll. Token never persists to
  // disk (NOT in readConfig); cleared after successful enroll.
  JsonArray en = FormBuilder::createForm(root, "enrollment",
                                          "Provision device with mothership");

  FormBuilder::addMessageField(en, "m_enroll_help",
      "First-time setup: generate a bootstrap token in the mothership "
      "admin UI (valid 24 hours), paste it below, click Enroll. The "
      "device will generate a keypair, send a CSR to the mothership, "
      "and store the signed certificate. After successful enrollment "
      "this tab can be ignored — the device authenticates via mTLS.",
      level("info"), icon("Info"));

  FormBuilder::addSecretField(en, "bootstrap_token", AF::RW,
                              s.bootstrap_token.c_str(),
                              label("Bootstrap token"),
                              placeholder("Paste token from mothership admin UI"),
                              icon("VpnKey"));

  // Enroll action — Phase 1.2 stub. POSTs the bootstrap_token via
  // withFields query param to the action endpoint; service-side
  // handler kicks off the enrollment flow (currently logs token and
  // flips state to Enrolling for one tick, then back to Failed with
  // a placeholder error — actual CSR + HTTPS POST in Phase 1.5).
  FormBuilder::addActionField(en, "enroll", "Enroll", AF::RW,
                              actionRef("cert.enroll"),
                              withFields("token=bootstrap_token"),
                              icon("Send"), color("primary"), refetchForm());

  FormBuilder::addMessageField(en, "m_enroll_warn",
      "If the device is already enrolled (state=Ready), running Enroll "
      "again will OVERWRITE the existing cert+key. Use only for first "
      "setup or after factory reset.",
      level("warning"), icon("Warning"));

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

  FormBuilder::addSwitchField(set, "auto_enroll", AF::RW, s.auto_enroll,
                              label("Auto-enroll on boot"),
                              icon("AutoMode"));

  // ── RECOVERY ────────────────────────────────────────────────────────
  // Phase 1.2 skeleton — Phase 4b fills in the polling status +
  // Approve/Reject feedback. For now just a placeholder so the tab
  // structure doesn't shift between phases.
  JsonArray rc = FormBuilder::createForm(root, "recovery",
                                          "Gray-zone recovery (lost cert)");

  FormBuilder::addMessageField(rc, "m_recovery_help",
      "If the device's cert was lost or expired while offline, this "
      "tab shows the recovery polling status. The device automatically "
      "contacts the mothership's /recover endpoint with its persisted "
      "recoveryToken and waits for an operator to approve re-enrollment "
      "from the admin UI. Implemented in Phase 4b — currently inactive.",
      level("info"), icon("Info"));

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

  // Enroll action — Phase 1.2 stub. Triggered by the "Enroll"
  // button in the Enrollment tab; reads bootstrap_token from the
  // request query (sent via withFields), kicks off enrollment.
  // Phase 1.5 will replace the stub body with real CSR generation
  // + HTTPS POST to the mothership /enroll endpoint.
  WebActionSpec enrollAct;
  enrollAct.id              = "cert.enroll";
  enrollAct.title           = "Enroll";
  enrollAct.icon            = "Send";
  enrollAct.color           = "primary";
  enrollAct.auth            = WebAuthLevel::Admin;
  enrollAct.successMessage  = "Enrollment kicked off";
  enrollAct.handler = [this](AsyncWebServerRequest* r) {
    String tok;
    if (r->hasArg("token")) tok = r->arg("token");
    tok.trim();
    Serial.printf("[cert.enroll] req token-len=%u, state=%u\n",
                  (unsigned)tok.length(),
                  (unsigned)_state.runtime_state);
    if (tok.length() == 0) {
      r->send(400, "application/json",
              "{\"ok\":false,\"err\":\"empty bootstrap token\"}");
      return;
    }
    if (!kickEnrollment(tok)) {
      r->send(409, "application/json",
              "{\"ok\":false,\"err\":\"enrollment already in progress\"}");
      return;
    }
    r->send(200, "application/json", "{\"ok\":true}");
  };
  web->registerAction(enrollAct);

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
    Serial.printf("[cert.begin] loaded CA bundle (%u B) into TLS context\n",
                  (unsigned)_state.ca_bundle_pem.length());
  }
  if (_tls && hasValidCert()) {
    _tls->updateClientCert(_state.device_cert_pem, _state.device_key_pem);
    Serial.printf("[cert.begin] loaded client cert (%u B) + key (%u B) into TLS context\n",
                  (unsigned)_state.device_cert_pem.length(),
                  (unsigned)_state.device_key_pem.length());
  }

  if (_feature) _feature->broadcastWs("boot");
}

void CertManagerService::loop() {
  // Auto-enroll loop. Fires kickEnrollment("") (no Bearer token →
  // server's path 6 parks the device in PENDING_ENROLLMENTS for
  // operator approval) when:
  //   * auto_enroll toggle is on (Settings tab)
  //   * runtime_state is NeedsEnrollment OR
  //     (Enrolling AND last attempt was >RETRY_S ago — the previous
  //     attempt got "pending" back, we want to re-poll for approval)
  //   * WiFi is up
  //   * No enrollment task currently running
  //   * Enough time elapsed since last attempt (rate-limit so we
  //     don't spam the mothership on a tight loop).
  //
  // Once the operator approves on the mothership admin page, the
  // next retry returns approved=true and the success path lands a
  // cert → state goes Ready → this branch falls through.
  using S = ICertProvider::State;
  if (!_state.auto_enroll) return;
  if (_enrollTask != nullptr) return;
  if (!WiFi.isConnected()) return;

  bool needs_attempt =
      (_state.runtime_state == S::NeedsEnrollment) ||
      (_state.runtime_state == S::Enrolling);
  if (!needs_attempt) return;

  // Rate-limit: 30 s between auto-attempts. Operator-initiated
  // kickEnrollment (typed bootstrap token) bypasses this — they go
  // through kickEnrollment directly, not through this loop.
  static constexpr uint32_t RETRY_S = 30;
  uint32_t now_s = (uint32_t)(millis() / 1000);
  if (now_s < _lastAutoEnrollAttempt_s + RETRY_S
      && _lastAutoEnrollAttempt_s != 0) {
    return;
  }
  _lastAutoEnrollAttempt_s = now_s;

  Serial.printf("[cert.auto-enroll] firing (state=%u)\n",
                (unsigned)_state.runtime_state);
  // Empty token → server's _handle_enroll path 6 (PENDING_ENROLLMENTS)
  // unless the deviceId is in EXPECTED_DEVICES (path 3, auto-trust).
  // kickEnrollment returns false if a task is already running, which
  // is fine — we'll retry on the next loop iteration.
  (void)kickEnrollment(String());
}

int32_t CertManagerService::daysUntilExpiry() const {
  if (_state.not_after_ts == 0) return INT32_MIN;
  uint32_t now_s = (uint32_t)(time(nullptr));
  if (now_s == 0) return 0;  // clock not set — treat as expired-soon
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
      Serial.printf("[cert.gen] ctr_drbg_seed failed: -0x%04x\n", -rc);
      break;
    }

    rc = mbedtls_pk_setup(&pk,
        mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (rc != 0) {
      Serial.printf("[cert.gen] pk_setup failed: -0x%04x\n", -rc);
      break;
    }

    rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                             mbedtls_pk_ec(pk),
                             mbedtls_ctr_drbg_random, &drbg);
    if (rc != 0) {
      Serial.printf("[cert.gen] ecp_gen_key failed: -0x%04x\n", -rc);
      break;
    }

    // PEM output buffer — 1024 B comfortably holds an EC P-256
    // private key PEM block (~250 B real content + base64 overhead).
    unsigned char pem_buf[1024];
    rc = mbedtls_pk_write_key_pem(&pk, pem_buf, sizeof(pem_buf));
    if (rc != 0) {
      Serial.printf("[cert.gen] pk_write_key_pem failed: -0x%04x\n", -rc);
      break;
    }

    outKeyPem = String((const char*)pem_buf);
    Serial.printf("[cert.gen] keypair generated, PEM=%u B\n",
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
    Serial.println("[cert.csr] empty keyPem");
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
      Serial.printf("[cert.csr] drbg seed failed: -0x%04x\n", -rc);
      break;
    }

    // mbedtls_pk_parse_key wants length INCLUDING the trailing NUL.
    rc = mbedtls_pk_parse_key(&pk,
        (const unsigned char*)keyPem.c_str(),
        keyPem.length() + 1,
        nullptr, 0,
        mbedtls_ctr_drbg_random, &drbg);
    if (rc != 0) {
      Serial.printf("[cert.csr] pk_parse_key failed: -0x%04x\n", -rc);
      break;
    }

    String subject = String("CN=") + deviceSubjectCN();
    rc = mbedtls_x509write_csr_set_subject_name(&csr, subject.c_str());
    if (rc != 0) {
      Serial.printf("[cert.csr] set_subject failed: -0x%04x\n", -rc);
      break;
    }

    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    mbedtls_x509write_csr_set_key(&csr, &pk);

    unsigned char pem_buf[1024];
    rc = mbedtls_x509write_csr_pem(
        &csr, pem_buf, sizeof(pem_buf),
        mbedtls_ctr_drbg_random, &drbg);
    if (rc != 0) {
      Serial.printf("[cert.csr] write_csr_pem failed: -0x%04x\n", -rc);
      break;
    }

    outCsrPem = String((const char*)pem_buf);
    Serial.printf("[cert.csr] CSR built, PEM=%u B, subject=%s\n",
                  (unsigned)outCsrPem.length(), subject.c_str());
    ok = true;
  } while (false);

  mbedtls_pk_free(&pk);
  mbedtls_x509write_csr_free(&csr);
  mbedtls_entropy_free(&entropy);
  mbedtls_ctr_drbg_free(&drbg);
  return ok;
}

// Subject CN = "device-<mac-hex-lowercase-no-sep>". Stable per-device,
// matches what server-side admin UI shows when reviewing pending
// enrollments. Used by buildCsr and exposed as a status field once
// the cert is signed.
String CertManagerService::deviceSubjectCN() const {
#if defined(ESP32)
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[24];
  snprintf(buf, sizeof(buf), "device-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
#else
  return String("device-unknown");
#endif
}

// ===== Phase 1.5 — Enrollment HTTPS POST =====
//
// kickEnrollment is called from the AsyncWebServer action thread —
// we just spawn a FreeRTOS task and return. Action handler responds
// 200 immediately so UI doesn't block on a 5-30 second handshake +
// CSR build.
//
// runEnrollment does the work in task context: keypair → CSR → POST
// → save → state transition. Updates state via update() so each
// transition fires WS broadcast and the operator sees live progress
// in the UI.

namespace {
struct EnrollArg {
  CertManagerService* svc;
  String              token;
};
}  // namespace

bool CertManagerService::kickEnrollment(const String& bootstrapToken) {
  if (_enrollTask != nullptr) {
    Serial.println("[cert.enroll] task already running");
    return false;
  }

  // Heap-owned arg — task is responsible for freeing it.
  auto* arg = new EnrollArg{this, bootstrapToken};

  // 8 KB stack — mbedtls keypair gen + CSR + HTTPS handshake +
  // ArduinoJson parsing on the same stack. ESP32 default tasks
  // are usually 4 KB; we double it to leave breathing room for
  // mbedtls scratch buffers.
  BaseType_t rc = xTaskCreatePinnedToCore(
      &CertManagerService::enrollTaskTramp,
      "certEnroll",
      8192,
      arg,
      1,
      &_enrollTask,
      tskNO_AFFINITY);

  if (rc != pdPASS) {
    Serial.printf("[cert.enroll] xTaskCreate failed: %d\n", (int)rc);
    delete arg;
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
  auto* a = static_cast<EnrollArg*>(arg);
  if (a && a->svc) a->svc->runEnrollment(a->token);
  if (a) delete a;
  // Task self-destruct: clear handle BEFORE vTaskDelete so parent
  // thread sees nullptr if it polls. The actual mutation must happen
  // through the service so it sees nullptr too.
  vTaskDelete(nullptr);
}

void CertManagerService::runEnrollment(const String& bootstrapToken) {
  Serial.println("[cert.enroll] task start");

  String keyPem;
  if (!generateEcdsaKeyPair(keyPem)) {
    update([](CertManagerSettings& s) {
      s.runtime_state = ICertProvider::State::EnrollmentFailed;
      s.status_label  = "Enrollment failed: keypair generation";
      s.bootstrap_token = String();
      return StateUpdateResult::CHANGED;
    }, "cert.enroll-fail");
    _enrollTask = nullptr;
    return;
  }

  String csrPem;
  if (!buildCsr(keyPem, csrPem)) {
    update([](CertManagerSettings& s) {
      s.runtime_state = ICertProvider::State::EnrollmentFailed;
      s.status_label  = "Enrollment failed: CSR build";
      s.bootstrap_token = String();
      return StateUpdateResult::CHANGED;
    }, "cert.enroll-fail");
    _enrollTask = nullptr;
    return;
  }

  String certPem, caBundlePem, recoveryToken;
  EnrollResult res = postCsrToMothership(csrPem, bootstrapToken,
                                           certPem, caBundlePem,
                                           recoveryToken);
  if (res == EnrollResult::Pending) {
    // Server parked us in its grey list — operator needs to approve
    // on /mothership admin. Stay in Enrolling state with a clear
    // status label; the auto-enroll loop in CertManagerService::loop
    // will retry on its own schedule. Don't wipe bootstrap_token —
    // operator's manual entry (if any) might still be the path that
    // matches a later regenerate.
    Serial.println("[cert.enroll] pending — operator approval needed");
    update([](CertManagerSettings& s) {
      s.runtime_state = ICertProvider::State::Enrolling;
      s.status_label  = "Awaiting operator approval on /mothership";
      return StateUpdateResult::CHANGED;
    }, "cert.enroll-pending");
    _enrollTask = nullptr;
    return;
  }
  if (res != EnrollResult::Ok) {
    update([](CertManagerSettings& s) {
      s.runtime_state = ICertProvider::State::EnrollmentFailed;
      s.status_label  = "Enrollment failed: server unreachable / rejected";
      s.bootstrap_token = String();
      return StateUpdateResult::CHANGED;
    }, "cert.enroll-fail");
    _enrollTask = nullptr;
    return;
  }

  String serialHex;
  uint32_t notAfterTs = 0;
  if (!parseCertMetadata(certPem, serialHex, notAfterTs)) {
    Serial.println("[cert.enroll] WARN: cert parse failed; proceeding "
                    "with empty serial / not_after");
    serialHex = "";
    notAfterTs = 0;
  }

  // Atomic write through update — all fields land together.
  update([&](CertManagerSettings& s) {
    s.device_cert_pem  = certPem;
    s.device_key_pem   = keyPem;
    s.ca_bundle_pem    = caBundlePem;
    s.serial_hex       = serialHex;
    s.subject_cn       = deviceSubjectCN();
    s.not_after_ts     = notAfterTs;
    s.recovery_token   = recoveryToken;
    s.bootstrap_token  = String();   // single-use → wipe
    s.runtime_state    = ICertProvider::State::Ready;
    s.status_label     = "Ready";
    return StateUpdateResult::CHANGED;
  }, "cert.enroll-ok");

  // Push fresh material into the TLS context so subsequent HTTPS
  // calls from any framework module pick up mTLS automatically.
  if (_tls) {
    _tls->updateClientCert(certPem, keyPem);
    _tls->loadCaChain(caBundlePem);  // for Phase 2 mothership /checkin
  }

  Serial.printf("[cert.enroll] done — serial=%s, not_after=%u\n",
                serialHex.c_str(), (unsigned)notAfterTs);
  _enrollTask = nullptr;
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

CertManagerService::EnrollResult CertManagerService::postCsrToMothership(
    const String& csrPem,
    const String& bootstrapToken,
    String& outCertPem,
    String& outCaBundlePem,
    String& outRecoveryToken) {
  outCertPem = String();
  outCaBundlePem = String();
  outRecoveryToken = String();

  String enroll_url = effectiveEnrollUrl();
  if (enroll_url.length() == 0) {
    Serial.println("[cert.enroll] no enroll URL — pick a Mothership "
                    "profile (or set PKI Base URL on the Settings tab)");
    return EnrollResult::Failed;
  }

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
    Serial.println("[cert.enroll] HTTPClient.begin failed");
    return EnrollResult::Failed;
  }
  http.setTimeout(20000);
  http.addHeader("Content-Type", "application/json");
  // Bearer header ONLY when the caller actually has a token. Empty
  // token = auto-enroll flow, server's path 6 parks the device in
  // its grey list for operator approval — sending "Bearer " would
  // trip the server's path-2 "Bearer present but invalid" 401 short-
  // circuit and never reach the pending-park branch.
  if (bootstrapToken.length() > 0) {
    http.addHeader("Authorization", String("Bearer ") + bootstrapToken);
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

  Serial.printf("[cert.enroll] POST %s, body=%u B\n",
                enroll_url.c_str(),
                (unsigned)reqBody.length());

  int code = http.POST(reqBody);
  Serial.printf("[cert.enroll] HTTP code=%d\n", code);

  if (code < 200 || code >= 300) {
    String err = http.getString();
    Serial.printf("[cert.enroll] server error: %s\n", err.c_str());
    http.end();
    return EnrollResult::Failed;
  }

  String respBody = http.getString();
  http.end();

  Serial.printf("[cert.enroll] response %u B\n", (unsigned)respBody.length());

  // Response is one of:
  //   approved=true  + cert_pem + ca_bundle_pem + recovery_token
  //       → EnrollResult::Ok, populate outputs
  //   approved=false + status=pending
  //       → EnrollResult::Pending, outputs stay empty; caller retries
  //   anything else
  //       → EnrollResult::Failed
  // 8 KB doc — cert PEM ~400 B, ca_bundle ~1.5 KB worst case, +
  // recovery_token + framing.
  DynamicJsonDocument resp(8192);
  DeserializationError jerr = deserializeJson(resp, respBody);
  if (jerr) {
    Serial.printf("[cert.enroll] JSON parse: %s\n", jerr.c_str());
    return EnrollResult::Failed;
  }

  bool approved = resp["approved"].as<bool>();
  if (!approved) {
    String status_str = resp["status"].as<String>();
    Serial.printf("[cert.enroll] server returned approved=false "
                  "status=%s — operator must approve on /mothership\n",
                  status_str.c_str());
    return EnrollResult::Pending;
  }

  outCertPem        = resp["cert_pem"].as<String>();
  outCaBundlePem    = resp["ca_bundle_pem"].as<String>();
  outRecoveryToken  = resp["recovery_token"].as<String>();

  if (outCertPem.length() == 0 || outCaBundlePem.length() == 0) {
    Serial.println("[cert.enroll] response missing cert_pem / ca_bundle_pem");
    return EnrollResult::Failed;
  }
  Serial.printf("[cert.enroll] parsed cert=%u ca=%u recovery=%u B\n",
                (unsigned)outCertPem.length(),
                (unsigned)outCaBundlePem.length(),
                (unsigned)outRecoveryToken.length());
  return EnrollResult::Ok;
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
    Serial.printf("[cert.parse] x509_crt_parse failed: -0x%04x\n", -rc);
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
