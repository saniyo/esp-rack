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
// Phase 1.0 SKELETON: minimal status surface. Phase 1.2 fills in
// the Enrollment tab with bootstrap-token field + "Enroll" action,
// the Status tab with cert details, and the Settings tab with the
// CA bundle uploader.
void CertManagerSettings::buildForm(CertManagerSettings& s, JsonObject& root) {
  // STATUS — single readout; Phase 1.2 expands.
  JsonArray st = FormBuilder::createForm(root, "status",
                                          "Device PKI status");

  FormBuilder::addTextField(st, "status", AF::R, s.status_label.c_str(),
                            label("State"), icon("VerifiedUser"),
                            colorMap("Ready:success,GrayZone:warning,"
                                     "EnrollmentFailed:error,default:info"));
  FormBuilder::addTextField(st, "subject_cn", AF::R, s.subject_cn.c_str(),
                            label("Subject CN"), icon("Badge"));
  FormBuilder::addTextField(st, "serial_hex", AF::R, s.serial_hex.c_str(),
                            label("Cert serial"), icon("Tag"));
  FormBuilder::addNumberField(st, "not_after_ts", AF::R,
                              (double)s.not_after_ts, format("0"),
                              label("notAfter (unix)"), icon("Schedule"));
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
