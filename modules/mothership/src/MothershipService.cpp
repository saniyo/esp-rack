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
// Phase 2.1: Status (RO live readouts) + Settings (RW URL / interval /
// enabled toggle). Phase 2.4 adds a command-log table to Status.
void MothershipSettings::buildForm(MothershipSettings& s, JsonObject& root) {
  // ── STATUS ───────────────────────────────────────────────────────
  JsonArray st = FormBuilder::createForm(root, "status",
                                          "Mothership client status");

  FormBuilder::addTextField(st, "status", AF::R, s.status_label.c_str(),
                            label("State"), icon("Cloud"),
                            colorMap("LastOk:success,CheckingIn:info,"
                                     "Idle:info,LastFail:error,"
                                     "NeedsCert:warning,Disabled:default,"
                                     "default:info"));

  // Derived "X seconds ago" / "next in X seconds" — computed at form-
  // render time from last_checkin_at_s + millis()/interval_min. WS
  // pushes refresh these without a full REST GET.
  uint32_t now_s = (uint32_t)(millis() / 1000);
  int32_t  last_ago = (s.last_checkin_at_s == 0)
                        ? -1
                        : (int32_t)(now_s - s.last_checkin_at_s);
  int32_t  next_in  = (s.next_checkin_at_s == 0 || !s.enabled)
                        ? -1
                        : (s.next_checkin_at_s > now_s
                            ? (int32_t)(s.next_checkin_at_s - now_s)
                            : 0);
  FormBuilder::addNumberField(st, "last_checkin_ago", AF::R,
                              (double)last_ago, format("0"),
                              label("Last check-in (s ago)"),
                              icon("Schedule"), unit("s"));
  FormBuilder::addNumberField(st, "next_checkin_in",  AF::R,
                              (double)next_in, format("0"),
                              label("Next check-in"),
                              icon("Update"), unit("s"));
  FormBuilder::addNumberField(st, "success_count", AF::R,
                              (double)s.success_count, format("0"),
                              label("Successful check-ins"),
                              icon("CheckCircleOutline"));
  FormBuilder::addNumberField(st, "fail_count", AF::R,
                              (double)s.fail_count, format("0"),
                              label("Failed check-ins"),
                              icon("ErrorOutline"));
  FormBuilder::addMessageField(st, "m_status_phase",
      "Phase 2.1 — settings UI is live. Phase 2.2 will start actual "
      "HTTPS check-ins; until then the counters stay at zero.",
      level("info"), icon("Info"));

  // ── SETTINGS ─────────────────────────────────────────────────────
  JsonArray set = FormBuilder::createForm(root, "settings",
                                           "Mothership client config");

  FormBuilder::addSwitchField(set, "enabled", AF::RW, s.enabled,
                              label("Enabled"), icon("PowerSettingsNew"));
  FormBuilder::addTextField(set, "checkin_url", AF::RW,
                            s.checkin_url.c_str(),
                            label("Check-in URL"),
                            placeholder(FACTORY_MOTHERSHIP_CHECKIN_URL),
                            icon("Cloud"));
  FormBuilder::addNumberField(set, "interval_min", AF::RW,
                              (double)s.interval_min,
                              minVal(1), maxVal(60), format("0"),
                              label("Check-in interval"),
                              icon("Timer"), unit("min"));
  FormBuilder::addMessageField(set, "m_settings_help",
      "Adaptive polling — when a check-in returns one or more "
      "actions the device drops to a 10-second burst cadence to "
      "drain the queue, then returns to this base interval. "
      "Default 5 min keeps load light on the mothership; tune to "
      "your fleet size.",
      level("info"), icon("Info"));
}

// ===== WS push =====
void MothershipSettings::staRead(MothershipSettings& s, JsonObject& root) {
  root["status"]        = s.status_label;
  root["success_count"] = s.success_count;
  root["fail_count"]    = s.fail_count;

  // Live countdown — recomputed every WS tick so the operator sees a
  // ticking next-checkin timer without refreshing the tab.
  uint32_t now_s = (uint32_t)(millis() / 1000);
  root["last_checkin_ago"] = (s.last_checkin_at_s == 0)
                                ? -1
                                : (int32_t)(now_s - s.last_checkin_at_s);
  root["next_checkin_in"]  = (s.next_checkin_at_s == 0 || !s.enabled)
                                ? -1
                                : (s.next_checkin_at_s > now_s
                                    ? (int32_t)(s.next_checkin_at_s - now_s)
                                    : 0);
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

  WebTabSpec settingsTab;
  settingsTab.key      = "settings";
  settingsTab.title    = "Settings";
  settingsTab.restPath = MOTHERSHIP_FORM_PATH;
  settingsTab.postable = true;
  settingsTab.auth     = WebAuthLevel::Admin;
  settingsTab.order    = 20;
  spec.tabs.push_back(settingsTab);

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
