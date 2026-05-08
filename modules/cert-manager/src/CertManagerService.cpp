#include <CertManagerService.h>
#include <ITLSProvider.h>
#include <WebManager.h>

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

  root["recovery_token"] = s.recovery_token;
  // bootstrap_token intentionally omitted — never persists.
}

StateUpdateResult CertManagerSettings::update(JsonObject& root,
                                              CertManagerSettings& s) {
  bool ch = false;
  ch |= FormBuilder::updateValue(root, "device_cert_pem", s.device_cert_pem);
  ch |= FormBuilder::updateValue(root, "device_key_pem",  s.device_key_pem);
  ch |= FormBuilder::updateValue(root, "ca_bundle_pem",   s.ca_bundle_pem);
  ch |= FormBuilder::updateValue(root, "serial_hex",      s.serial_hex);
  ch |= FormBuilder::updateValue(root, "subject_cn",      s.subject_cn);
  ch |= FormBuilder::updateValue(root, "not_after_ts",    s.not_after_ts);
  ch |= FormBuilder::updateValue(root, "recovery_token",  s.recovery_token);
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
           4096,
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
    Serial.printf("[cert.enroll] token-len=%u, current state=%u\n",
                  (unsigned)tok.length(),
                  (unsigned)_state.runtime_state);
    if (tok.length() == 0) {
      r->send(400, "application/json",
              "{\"ok\":false,\"err\":\"empty bootstrap token\"}");
      return;
    }
    // Phase 1.5: enqueue real enrollment (gen keypair → CSR → POST).
    // For now flip state to Enrolling, log + revert next loop tick
    // so operator sees the wiring is alive end-to-end without an
    // actual server.
    update([tok](CertManagerSettings& s) {
      s.bootstrap_token   = tok;
      s.runtime_state     = ICertProvider::State::Enrolling;
      s.status_label      = "Enrolling (stub — phase 1.5 will do real work)";
      return StateUpdateResult::CHANGED;
    }, "cert.enroll-start");
    r->send(200, "application/json", "{\"ok\":true,\"stub\":true}");
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

  WebTabSpec recoveryTab;
  recoveryTab.key      = "recovery";
  recoveryTab.title    = "Recovery";
  recoveryTab.restPath = CERT_MANAGER_FORM_PATH;
  recoveryTab.postable = false;
  recoveryTab.live     = true;
  recoveryTab.auth     = WebAuthLevel::Admin;
  recoveryTab.order    = 20;
  spec.tabs.push_back(recoveryTab);

  _feature = web->registerFeature<CertManagerSettings>(
      std::move(spec), this,
      CertManagerSettings::buildForm,  CertManagerSettings::update,
      CertManagerSettings::staRead,    CertManagerSettings::staUpd,
      8192, 4096);
}

void CertManagerService::begin() {
  (void)_cfg.ensureLoaded();
  refreshRuntimeState();

  // Phase 1.1: push loaded cert into TLS context so all subsequent
  // attachToClient() calls get the chain. For now no-op.
  if (_tls && hasValidCert()) {
    _tls->updateClientCert(_state.device_cert_pem, _state.device_key_pem);
  }
  if (_tls && _state.ca_bundle_pem.length() > 0) {
    // _tls->loadCaChain(_state.ca_bundle_pem);  // wired in Phase 1.1 via concrete TLSContextService
  }

  if (_feature) _feature->broadcastWs("boot");
}

void CertManagerService::loop() {
  // Phase 1.0: nothing to do. Phases 1.5+ add enrollment retry
  // backoff and Phase 4b adds gray-zone /recover polling here.
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
