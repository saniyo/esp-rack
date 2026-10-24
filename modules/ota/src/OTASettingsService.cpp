#include <OTASettingsService.h>

#include <App.h>  // ESPRack::restartDevice
#include <system/FirmwareUpdate.h>
#include <system/LoopWdt.h>
#include <web/Form.h>
#include <WebManager.h>

void OTASettings::buildForm(OTASettings& settings, JsonObject& root) {
  web::Form form(root, "ota", "OTA Settings");
  form.add(web::Switch("enabled", settings.enabled).icon("SystemUpdate"));
  form.add(web::Number("port", (double)settings.port)
               .min(1).max(65535).format("0").icon("Settings"));
  form.add(web::Secret("pwd", settings.password.c_str()).icon("Lock"));
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
  // POST /api/v1/ota/state → update() applies state → this handler
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
  spec.auth = WebAuthLevel::Admin;
  // restRead / restUpdate omitted — registry mounts /api/v1/ota/form
  // and /api/v1/ota/state automatically.

  // Auto-sized: three scalar fields, measured 216 B live, no runtime rows.
  _feature = web->registerFeature<OTASettings>(
      std::move(spec), this,
      OTASettings::buildForm, OTASettings::update,
      WEBFEATURE_BUFFER_AUTO);

  WebTabSpec tab;
  tab.key = "ota";
  tab.title = "OTA Settings";
  // Contributed to "system" — reference our typed feature by id; the
  // manifest emitter resolves the derived /api/v1/ota/form path.
  tab.formOf = "ota";
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
    log_d("Starting OTA Update Service...");
    _arduinoOTA = new ArduinoOTAClass;
    // Do NOT let ArduinoOTA start its own mDNS responder. Its begin()
    // unconditionally calls MDNS.begin() when _mdnsEnabled (default true),
    // which spawns the IDF "mdns" task + responder heap (~12-15 KB on the
    // no-PSRAM C3). The framework no longer ships an mDNS responder at all
    // (device discovery moved to the UDP DiscoveryService), so OTA must not
    // resurrect one. Upload still works by IP (espota / PlatformIO target the
    // device IP); <host>.local resolves via lwIP's native one-shot mDNS query
    // (CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES) without any responder.
    _arduinoOTA->setMdnsEnabled(false);
    _arduinoOTA->setPort(_state.port);
    _arduinoOTA->setPassword(_state.password.c_str());
    // The transfer blocks the loop for the whole upload: pause the loop
    // watchdog until it ends or fails.
    _arduinoOTA->onStart([]() {
      Serial.println(F("Starting"));
      ESPRack::LoopWdt::pause();
      ESPRack::FirmwareUpdate::begin("ota");
    });
    // Reboot through the framework: the modules' onShutdown()
    // runs on the loop task and the next boot reports lastRestartReason()
    // "ota". ArduinoOTA has already answered OK when onEnd runs.
    _arduinoOTA->setRebootOnSuccess(false);
    _arduinoOTA->onEnd([]() {
      Serial.println(F("\r\nEnd"));
      ESPRack::FirmwareUpdate::end();
      ESPRack::restartDevice(ESPRack::kRestartOta);
    });
    _arduinoOTA->onProgress([](unsigned int progress, unsigned int total) {
      ESPRack::FirmwareUpdate::progress();
      log_d("Progress: %u%%", (progress / (total / 100)));
    });
    _arduinoOTA->onError([](ota_error_t error) {
      ESPRack::FirmwareUpdate::end();
      ESPRack::LoopWdt::resume();
      log_d("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR)
        log_d("Auth Failed");
      else if (error == OTA_BEGIN_ERROR)
        log_d("Begin Failed");
      else if (error == OTA_CONNECT_ERROR)
        log_d("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR)
        log_d("Receive Failed");
      else if (error == OTA_END_ERROR)
        log_d("End Failed");
    });
    _arduinoOTA->begin();
  }
}

#ifdef ESP32
void OTASettingsService::onStationModeGotIP(WiFiEvent_t event, WiFiEventInfo_t info) {
  configureArduinoOTA();
}
#endif
