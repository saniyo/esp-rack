#include <NTPSettingsService.h>
#include <WebManager.h>

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
    configureNTP();
    _cfg.saveIfChanged(origin);
    if (_feature) _feature->broadcastWs(origin);
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
  spec.restRead   = NTP_FORM_PATH;
  spec.restUpdate = NTP_FORM_PATH;
  spec.wsPath     = NTP_WS_PATH;

  WebTabSpec statusTab;
  statusTab.key      = "status";
  statusTab.title    = "Status";
  statusTab.restPath = NTP_FORM_PATH;
  statusTab.postable = false;
  statusTab.live     = true;
  statusTab.auth     = WebAuthLevel::Authenticated;
  spec.tabs.push_back(statusTab);

  WebTabSpec settingsTab;
  settingsTab.key      = "settings";
  settingsTab.title    = "Settings";
  settingsTab.restPath = NTP_FORM_PATH;
  settingsTab.postable = true;
  settingsTab.auth     = WebAuthLevel::Admin;
  spec.tabs.push_back(settingsTab);

  _feature = web->registerFeature<NTPSettings>(
      std::move(spec), this,
      NTPSettings::buildForm, NTPSettings::update,
      NTPSettings::staRead,   NTPSettings::staUpd,
      2048, 2048);
}

void NTPSettingsService::begin() {
  (void)_cfg.ensureLoaded();
  configureNTP();
  if (_feature) _feature->broadcastWs("boot");
}

void NTPSettingsService::loop() {
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

#ifdef ESP32
void NTPSettingsService::onStationModeGotIP(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.println(F("Got IP address, starting NTP synchronization."));
  configureNTP();
}
void NTPSettingsService::onStationModeDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.println(F("WiFi dropped, stopping NTP."));
  configureNTP();
}
#elif defined(ESP8266)
void NTPSettingsService::onStationModeGotIP(const WiFiEventStationModeGotIP& event) {
  configureNTP();
}
void NTPSettingsService::onStationModeDisconnected(const WiFiEventStationModeDisconnected& event) {
  configureNTP();
}
#endif

void NTPSettingsService::configureNTP() {
  if (WiFi.isConnected() && _state.enabled) {
    Serial.println(F("Starting NTP..."));
#ifdef ESP32
    configTzTime(_state.tzFormat.c_str(), _state.server.c_str());
#elif defined(ESP8266)
    configTime(_state.tzFormat.c_str(), _state.server.c_str());
#endif
  } else {
#ifdef ESP32
    setenv("TZ", _state.tzFormat.c_str(), 1);
    tzset();
#elif defined(ESP8266)
    setTZ(_state.tzFormat.c_str());
#endif
    // IDF 5 panics with "Required to lock TCPIP core functionality!"
    // when sntp_stop() is called bare from a non-tcpip thread.
    // WIFI_SAFE_SNTP_STOP() routes to esp_sntp_stop() (locked wrapper)
    // on IDF 5+ and to sntp_stop() on older cores where the bare call
    // is still safe.
    WIFI_SAFE_SNTP_STOP();
  }
}
