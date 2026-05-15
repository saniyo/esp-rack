#include <OTASettingsService.h>

#include <FormBuilder.h>
#include <WebManager.h>

void OTASettings::buildForm(OTASettings& settings, JsonObject& root) {
  JsonArray set = FormBuilder::createForm(root, "ota", "OTA Settings");
  FormBuilder::addSwitchField(set, "enabled", AF::RW, settings.enabled,
                              icon("SystemUpdate"));
  FormBuilder::addNumberField(set, "port", AF::RW, (double)settings.port,
                              minVal(1), maxVal(65535), format("0"),
                              icon("Settings"));
  FormBuilder::addSecretField(set, "pwd", AF::RW, settings.password.c_str(),
                              icon("Lock"));
}

OTASettingsService::OTASettingsService(AsyncWebServer* server,
                                       ConfigManager* cfgMgr,
                                       SecurityManager* securityManager) :
    _httpEndpoint(OTASettings::read, OTASettings::update, this, server, OTA_SETTINGS_SERVICE_PATH, securityManager),
    _formEndpoint(OTASettings::buildForm, OTASettings::update, this, server, OTA_SETTINGS_FORM_PATH, securityManager,
                  AuthenticationPredicates::IS_ADMIN),
    _cfg(cfgMgr,
         "ota",
         OTA_SETTINGS_FILE,
         1024,
         this,
         OTASettings::readConfig,
         OTASettings::update,
         false /*autoSave*/,
         nullptr /*validator*/,
         OTASettings::buildForm /*formReader*/) {
#ifdef ESP32
  WiFi.onEvent(std::bind(&OTASettingsService::onStationModeGotIP, this, std::placeholders::_1, std::placeholders::_2),
               WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
#endif
  // POST /rest/otaSettings or /rest/ota/settings → update() applies state →
  // this handler runs synchronously after the response is queued (see
  // HttpEndpoint sync-fix), reconfigures ArduinoOTA, and persists via
  // ConfigManager (snapshot rotation handled by atomicWrite). OTA now
  // shows up in the Config Manager tab alongside every other config.
  addUpdateHandler([this](const String& origin) {
    configureArduinoOTA();
    _cfg.saveIfChanged(origin);
  }, false);
}

void OTASettingsService::registerManifest(WebManager* web) {
  if (!web) return;
  WebTabSpec tab;
  tab.key = "ota";
  tab.title = "OTA Settings";
  tab.restPath = OTA_SETTINGS_FORM_PATH;
  tab.postable = true;
  tab.auth = WebAuthLevel::Admin;
  tab.order = 60;
  web->addTabToFeature("system", tab);

  // Phase 7c — mship-ui proxy reach for the typed config endpoint.
  _httpEndpoint.registerProxy(web, OTA_SETTINGS_SERVICE_PATH);
  _formEndpoint.registerProxy(web, OTA_SETTINGS_FORM_PATH);
}

void OTASettingsService::begin() {
  (void)_cfg.ensureLoaded();
  configureArduinoOTA();
}

void OTASettingsService::loop() {
  if (_state.enabled && _arduinoOTA) {
    _arduinoOTA->handle();
  }
}

void OTASettingsService::configureArduinoOTA() {
  if (_arduinoOTA) {
#ifdef ESP32
    _arduinoOTA->end();
#endif
    delete _arduinoOTA;
    _arduinoOTA = nullptr;
  }
  if (_state.enabled) {
    // Defer ArduinoOTA::begin() until STA actually has an IP. On
    // Arduino-3.x / IDF 5 (ESP32-C6) creating the OTA UDP socket before
    // lwIP is ready asserts in xQueueSemaphoreTake() — the network task's
    // semaphore handle is still NULL. The GOT_IP event handler below
    // re-runs this routine once the stack is up, so an early skip is
    // harmless on every core. Arduino-2.x was tolerant; 3.x panics.
#ifdef ESP32
    if (!WiFi.isConnected()) {
      return;
    }
#endif
    Serial.println(F("Starting OTA Update Service..."));
    _arduinoOTA = new ArduinoOTAClass;
    _arduinoOTA->setPort(_state.port);
    _arduinoOTA->setPassword(_state.password.c_str());
    _arduinoOTA->onStart([]() { Serial.println(F("Starting")); });
    _arduinoOTA->onEnd([]() { Serial.println(F("\r\nEnd")); });
    _arduinoOTA->onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf_P(PSTR("Progress: %u%%\r\n"), (progress / (total / 100)));
    });
    _arduinoOTA->onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR)
        Serial.println(F("Auth Failed"));
      else if (error == OTA_BEGIN_ERROR)
        Serial.println(F("Begin Failed"));
      else if (error == OTA_CONNECT_ERROR)
        Serial.println(F("Connect Failed"));
      else if (error == OTA_RECEIVE_ERROR)
        Serial.println(F("Receive Failed"));
      else if (error == OTA_END_ERROR)
        Serial.println(F("End Failed"));
    });
    _arduinoOTA->begin();
  }
}

#ifdef ESP32
void OTASettingsService::onStationModeGotIP(WiFiEvent_t event, WiFiEventInfo_t info) {
  configureArduinoOTA();
}
#endif
