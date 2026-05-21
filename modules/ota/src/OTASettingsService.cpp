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

OTASettingsService::OTASettingsService(ConfigManager* cfgMgr) :
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
  // POST /rest/ota/settings → update() applies state → this handler
  // runs after the response is queued, reconfigures ArduinoOTA, and
  // persists via ConfigManager. OTA lives in the Config Manager tab
  // alongside every other config.
  addUpdateHandler([this](const String& origin) {
    configureArduinoOTA();
    _cfg.saveIfChanged(origin);
  }, false);
}

void OTASettingsService::registerManifest(WebManager* web) {
  if (!web) return;

  // OTA is a sub-tab of the compound `system` feature. We still
  // register a feature so WebManager owns the endpoint binding + auth
  // wrapper + proxy reach (same pipeline as wifi / ntp / mqtt).
  // No menu entry: the system feature owns the visible menu slot.
  WebFeatureSpec spec;
  spec.id         = "ota";
  spec.title      = "OTA Settings";
  spec.component  = "";  // sub-tab, system feature provides the page
  spec.auth       = WebAuthLevel::Admin;
  spec.restRead   = OTA_SETTINGS_FORM_PATH;
  spec.restUpdate = OTA_SETTINGS_FORM_PATH;

  _feature = web->registerFeature<OTASettings>(
      std::move(spec), this,
      OTASettings::buildForm, OTASettings::update,
      2048);

  WebTabSpec tab;
  tab.key = "ota";
  tab.title = "OTA Settings";
  tab.restPath = OTA_SETTINGS_FORM_PATH;
  tab.postable = true;
  tab.auth = WebAuthLevel::Admin;
  tab.order = 60;
  web->addTabToFeature("system", tab);
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
