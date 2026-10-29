#include <NTPSettingsService.h>
#include <WebManager.h>
#include <App.h>  // ESPRack::wakeLoop
#include <system/LoopWait.h>

NTPSettingsService::NTPSettingsService(ConfigManager* cfgMgr)
    : StatefulService<NTPSettings>(),
      _cfg(cfgMgr,
           "ntp",
           NTP_FILE,
           2048,
           this,
           NTPSettings::readConfig,
           NTPSettings::update,
           false /*autoSave*/) {
  // NTP is a time source of core TimeSync. With the module installed (even
  // disabled) the clock counts as sane only once a source made it valid.
  ESPRack::TimeSync::instance().registerSource(ESPRack::TimeSource::Ntp);
#ifdef ESP32
  WiFi.onEvent(
      std::bind(&NTPSettingsService::onStationModeDisconnected, this, std::placeholders::_1, std::placeholders::_2),
      WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent(
      std::bind(&NTPSettingsService::onStationModeGotIP, this, std::placeholders::_1, std::placeholders::_2),
      WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
#elif defined(ESP8266)
  _onStationModeDisconnectedHandler = WiFi.onStationModeDisconnected(
      std::bind(&NTPSettingsService::onStationModeDisconnected, this, std::placeholders::_1));
  _onStationModeGotIPHandler =
      WiFi.onStationModeGotIP(std::bind(&NTPSettingsService::onStationModeGotIP, this, std::placeholders::_1));
#endif

  addUpdateHandler([this](const String& origin) {
    requestConfigure();
    _cfg.saveIfChanged(origin);
    // No broadcastWs here (LIB-FIX-009): the feature's WS binding pushes on
    // every propagated update, after this handler has refreshed state.
  }, false);
}

void NTPSettingsService::registerManifest(WebManager* web) {
  if (!web) return;

  WebFeatureSpec spec;
  spec.id         = "ntp";
  spec.title      = "Network Time";
  spec.component  = "DynamicSettings";
  spec.menu.label = "Network Time";
  spec.menu.icon  = "AccessTime";
  spec.menu.order = 200;
  spec.menu.auth  = WebAuthLevel::Authenticated;
  spec.auth       = WebAuthLevel::Authenticated;
  // Visible to every user, changeable only by admins - enforced server-side
  // on POST /state and WS writes (LIB-FIX-011).
  spec.hasWriteAuth = true;
  spec.writeAuth    = WebAuthLevel::Admin;
  // restRead/restUpdate are no longer set: the registry derives the typed
  // feature's mount paths from spec.id — /api/v1/ntp/form (GET) and
  // /api/v1/ntp/state (GET+POST). WS path stays explicit (/ws/*).
  spec.wsPath     = NTP_WS_PATH;

  // Own-feature tabs leave restPath null — toJson emits the derived form
  // path (/api/v1/ntp/form) for them.
  WebTabSpec statusTab;
  statusTab.key      = "status";
  statusTab.title    = "Status";
  statusTab.postable = false;
  statusTab.live     = true;
  statusTab.auth     = WebAuthLevel::Authenticated;
  spec.tabs.push_back(statusTab);

  WebTabSpec settingsTab;
  settingsTab.key      = "settings";
  settingsTab.title    = "Settings";
  settingsTab.postable = true;
  settingsTab.auth     = WebAuthLevel::Admin;
  spec.tabs.push_back(settingsTab);

  // REST buffer auto-sized (measured 0.6 KB live; clock strings are
  // fixed-width strftime output, no runtime rows). WS stays a fixed
  // 2048 — staRead is a flat scalar payload, not guard-coupled.
  _feature = web->registerFeature<NTPSettings>(
      std::move(spec), this,
      NTPSettings::buildForm, NTPSettings::update,
      NTPSettings::staRead,   NTPSettings::staUpd,
      WEBFEATURE_BUFFER_AUTO, 2048);
}

void NTPSettingsService::begin() {
  (void)_cfg.ensureLoaded();
  configureNTP();
  if (_feature) _feature->broadcastWs("boot");
}

void NTPSettingsService::loop() {
  if (_reconfigure) {
    _reconfigure = false;  // before the work: a request meanwhile runs it again
    configureNTP();
  }
  // Periodic push so the Status tab (local/utc time, uptime) actually ticks.
  // Skip ALL work — even the millis() diff — when nobody's subscribed; the
  // unconditional 1 Hz tick was previously feeding into main-loop pressure
  // and starving LightControl's WS on busy AP-networks.
  if (!_feature || !_feature->hasSubscribers()) return;
  unsigned long now = millis();
  if (now - _lastTickMs >= 1000UL) {
    _lastTickMs = now;
    _feature->broadcastWs("tick");
  }
}

uint32_t NTPSettingsService::loopWaitMs(uint32_t nowMs) const {
  if (_reconfigure) return 0;
  if (!_feature || !_feature->hasSubscribers()) return ESPRACK_LOOP_IDLE_WAIT_MS;
  return ESPRack::LoopWait::dueIn(nowMs, (uint32_t)_lastTickMs, 1000);
}

void NTPSettingsService::requestConfigure() {
  _reconfigure = true;
  ESPRack::wakeLoop();
}

#ifdef ESP32
void NTPSettingsService::onStationModeGotIP(WiFiEvent_t event, WiFiEventInfo_t info) {
  log_d("Got IP address, starting NTP synchronization.");
  requestConfigure();
}
void NTPSettingsService::onStationModeDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  log_d("WiFi dropped, stopping NTP.");
  requestConfigure();
}
#elif defined(ESP8266)
void NTPSettingsService::onStationModeGotIP(const WiFiEventStationModeGotIP& event) {
  requestConfigure();
}
void NTPSettingsService::onStationModeDisconnected(const WiFiEventStationModeDisconnected& event) {
  requestConfigure();
}
#endif

void NTPSettingsService::configureNTP() {
#ifdef ESP32
  // On the loop task only, so the calls are serialized: a copy of the
  // settings under the lock, the slow part (TimeSync, SNTP via tcpip) after
  // it, so no reader waits on the tcpip task through the lock.
  // The state lock timed out: keep what runs, and try again next pass.
  NTPSettings st;
  if (!read([&st](NTPSettings& s) { st = s; })) {
    _reconfigure = true;
    return;
  }
  applySntp(st);
#elif defined(ESP8266)
  if (WiFi.isConnected() && _state.enabled) {
    configTime(_state.tzFormat.c_str(), _state.server.c_str());
  } else {
    setTZ(_state.tzFormat.c_str());
    WIFI_SAFE_SNTP_STOP();
  }
#endif
}

#ifdef ESP32
void NTPSettingsService::applySntp(const NTPSettings& st) {
  // The timezone belongs to core TimeSync (libc TZ and everything that reads it), not
  // to the SNTP client: set it whether or not NTP runs.
  if (!ESPRack::TimeSync::instance().setTimezone(st.tzFormat.c_str(), st.tzLabel.c_str())) {
    log_w("[ntp] timezone '%s' rejected, keeping the previous one", st.tzFormat.c_str());
  }
  const bool run = WiFi.isConnected() && st.enabled;
  // SNTP results go through TimeSync (its sntp_sync_time() hook), which
  // decides whether they may step the clock.
  //
  // The esp_sntp_* wrappers are the thread-safe IDF 5 API: setservername
  // runs through tcpip_api_call, which takes the (non-recursive) TCPIP core
  // lock itself, and init/stop are queued to the tcpip thread with
  // tcpip_callback. So they must be called WITHOUT holding that lock
  // (holding it deadlocks), and stop/init execute later, in call order.
  // setoperatingmode is not called: POLL is lwIP's default, and setting the
  // mode while the queued stop hasn't run yet would trip lwIP's assert.
  //
  // The stop is unconditional: esp_sntp_enabled() can't see an init that is
  // still queued, and a queued stop runs after it (FIFO), so the client
  // always ends up in the state of the last call.
  esp_sntp_stop();
  if (run) {
    log_d("[ntp] starting SNTP (%s)", st.server.c_str());
    // lwIP keeps the pointer; the client may still read the previous name
    // until the queued stop runs, so write the other buffer.
    _serverSel ^= 1;
    strlcpy(_serverBuf[_serverSel], st.server.c_str(), sizeof(_serverBuf[0]));
    esp_sntp_setservername(0, _serverBuf[_serverSel]);
    esp_sntp_set_sync_interval(NTP_SYNC_INTERVAL_MS);
    esp_sntp_init();
  }
}
#endif
