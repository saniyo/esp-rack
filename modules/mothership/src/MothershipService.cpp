#include <MothershipService.h>
#include <ITLSProvider.h>
#include <ICertProvider.h>
#include <WebManager.h>

// ===== Persistence =====

void MothershipSettings::readConfig(MothershipSettings& s, JsonObject& root) {
  root["enabled"]      = s.enabled;
  root["checkin_url"]  = s.checkin_url;
  root["interval_min"] = s.interval_min;
}

StateUpdateResult MothershipSettings::update(JsonObject& root,
                                              MothershipSettings& s) {
  bool ch = false;
  ch |= FormBuilder::updateValue(root, "enabled",      s.enabled);
  ch |= FormBuilder::updateValue(root, "checkin_url",  s.checkin_url);
  ch |= FormBuilder::updateValue(root, "interval_min", s.interval_min);
  return ch ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
}

// ===== Form schema =====
// Phase 2.0 skeleton — single Status tab placeholder. Phase 2.1
// expands with Settings tab (URL / interval / enabled toggle) +
// Phase 2.4 adds command log table.
void MothershipSettings::buildForm(MothershipSettings& s, JsonObject& root) {
  JsonArray st = FormBuilder::createForm(root, "status",
                                          "Mothership client status");

  FormBuilder::addTextField(st, "status", AF::R, s.status_label.c_str(),
                            label("State"), icon("Cloud"),
                            colorMap("LastOk:success,CheckingIn:info,"
                                     "Idle:info,LastFail:error,"
                                     "NeedsCert:warning,Disabled:default,"
                                     "default:info"));
  FormBuilder::addNumberField(st, "success_count", AF::R,
                              (double)s.success_count, format("0"),
                              label("Successful check-ins"),
                              icon("CheckCircleOutline"));
  FormBuilder::addNumberField(st, "fail_count", AF::R,
                              (double)s.fail_count, format("0"),
                              label("Failed check-ins"),
                              icon("ErrorOutline"));
  FormBuilder::addMessageField(st, "m_status_help",
      "Phase 2.0 skeleton — settings UI (Phase 2.1) + actual HTTPS "
      "check-in (Phase 2.2) + command dispatcher (Phase 2.3) arrive "
      "in subsequent commits. Currently the module is wired but does "
      "no network work.",
      level("info"), icon("Info"));
}

// ===== WS push =====
void MothershipSettings::staRead(MothershipSettings& s, JsonObject& root) {
  root["status"]        = s.status_label;
  root["success_count"] = s.success_count;
  root["fail_count"]    = s.fail_count;
}

StateUpdateResult MothershipSettings::staUpd(JsonObject& root,
                                              MothershipSettings& s) {
  return update(root, s);
}

// ===== Service =====

MothershipService::MothershipService(ConfigManager* cfgMgr,
                                     ITLSProvider* tls,
                                     ICertProvider* cert)
    : StatefulService<MothershipSettings>(),
      _cfg(cfgMgr,
           "mothership",
           MOTHERSHIP_FILE,
           4096,
           this,
           MothershipSettings::readConfig,
           MothershipSettings::update,
           false /*autoSave*/,
           nullptr /*validator*/,
           MothershipSettings::buildForm /*formReader*/),
      _tls(tls),
      _cert(cert) {
  addUpdateHandler([this](const String& origin) {
    refreshRuntimeState();
    _cfg.saveIfChanged(origin);
    if (_feature) _feature->broadcastWs(origin);
  }, false);
}

void MothershipService::registerManifest(WebManager* web) {
  if (!web) return;

  WebFeatureSpec spec;
  spec.id         = "mothership";
  spec.title      = "Mothership";
  spec.component  = "DynamicSettings";
  spec.menu.label = "Mothership";
  spec.menu.icon  = "Cloud";
  spec.menu.order = 370;
  spec.menu.auth  = WebAuthLevel::Admin;
  spec.auth       = WebAuthLevel::Admin;
  spec.restRead   = MOTHERSHIP_FORM_PATH;
  spec.restUpdate = MOTHERSHIP_FORM_PATH;
  spec.wsPath     = MOTHERSHIP_WS_PATH;

  WebTabSpec statusTab;
  statusTab.key      = "status";
  statusTab.title    = "Status";
  statusTab.restPath = MOTHERSHIP_FORM_PATH;
  statusTab.postable = false;
  statusTab.live     = true;
  spec.tabs.push_back(statusTab);

  _feature = web->registerFeature<MothershipSettings>(
      std::move(spec), this,
      MothershipSettings::buildForm,  MothershipSettings::update,
      MothershipSettings::staRead,    MothershipSettings::staUpd,
      8192, 4096);
}

void MothershipService::begin() {
  (void)_cfg.ensureLoaded();
  refreshRuntimeState();
  if (_feature) _feature->broadcastWs("boot");
}

void MothershipService::loop() {
  // Phase 2.2 will spawn a FreeRTOS check-in task. For now no-op.
}

int32_t MothershipService::lastCheckInAgoSec() const {
  if (_state.last_checkin_at_s == 0) return INT32_MIN;
  uint32_t now_s = (uint32_t)(millis() / 1000);
  if (now_s < _state.last_checkin_at_s) return 0;
  return (int32_t)(now_s - _state.last_checkin_at_s);
}

int32_t MothershipService::nextCheckInInSec() const {
  if (!_state.enabled) return INT32_MIN;
  if (_state.runtime_state == State::CheckingIn) return 0;
  uint32_t now_s = (uint32_t)(millis() / 1000);
  if (_state.next_checkin_at_s <= now_s) return 0;
  return (int32_t)(_state.next_checkin_at_s - now_s);
}

void MothershipService::refreshRuntimeState() {
  using S = IMothershipProvider::State;
  S newState;

  if (!_state.enabled) {
    newState = S::Disabled;
  } else if (!_cert || !_cert->hasValidCert()) {
    // Module is on but device doesn't have a usable cert yet —
    // operator must finish enrollment before mothership can talk
    // through mTLS.
    newState = S::NeedsCert;
  } else if (_state.runtime_state == S::CheckingIn) {
    // Don't clobber transient in-flight state on every refresh.
    newState = S::CheckingIn;
  } else if (_state.fail_count > _state.success_count
             && _state.fail_count > 0) {
    newState = S::LastFail;
  } else if (_state.success_count > 0) {
    newState = S::LastOk;
  } else {
    newState = S::Idle;
  }

  _state.runtime_state = newState;
  switch (newState) {
    case S::Disabled:    _state.status_label = "Disabled";    break;
    case S::NeedsCert:   _state.status_label = "NeedsCert";   break;
    case S::Idle:        _state.status_label = "Idle";        break;
    case S::CheckingIn:  _state.status_label = "CheckingIn";  break;
    case S::LastOk:      _state.status_label = "LastOk";      break;
    case S::LastFail:    _state.status_label = "LastFail";    break;
  }
}
