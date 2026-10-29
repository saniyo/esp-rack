#include <OTASettingsService.h>

#include <App.h>  // ESPRack::restartDevice
#include <power/WriteGate.h>
#include <system/FirmwareUpdate.h>
#include <system/LoopWdt.h>
#ifdef ESP32
#include <system/OtaWriter.h>
#endif
#include <web/Form.h>
#include <WebManager.h>
#include <system/LoopWait.h>

namespace {

// ArduinoOTA's transfer holds the firmware update (the loop task only).
bool s_otaOwn = false;
const char kOtaSource[] = "ota";  // FirmwareUpdate's name for it: end() checks the pointer

// ArduinoOTA's onStart: the update for this transfer, or false (another
// writer has it: the upload page, auto-update; they share the next slot).
bool takeUpdate() {
#ifdef ESP32
  // An OtaWriter takes its claim, then FirmwareUpdate: one that comes
  // between these two finds FirmwareUpdate taken and backs off.
  if (ESPRack::OtaWriter::busy() || !ESPRack::FirmwareUpdate::tryBegin(kOtaSource)) return false;
#else
  ESPRack::FirmwareUpdate::begin(kOtaSource);
#endif
  return true;
}

// The transfer ended or failed.
void giveUpdate(bool resumeWdt) {
  if (!s_otaOwn) return;
  s_otaOwn = false;
  (void)ESPRack::FirmwareUpdate::end(kOtaSource);  // ours only: a stale one may have been taken over
  if (resumeWdt) ESPRack::LoopWdt::resume();
}

}  // namespace

void OTASettings::buildForm(OTASettings& settings, JsonObject& root) {
  web::Form form(root, "ota", "OTA Settings");
  if (ESPRack::WriteGate::holders() > 0) {
    // Arduino's Update erases 64 KB blocks: not through the write gate.
    form.add(web::Message("m_gate",
                          "A module on this device holds flash write windows. ArduinoOTA writes past them "
                          "(64 KB block erases); prefer the firmware upload page or auto-update.")
                 .level("warning"));
  }
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
  // runs after the response is queued, has loop() reconfigure ArduinoOTA,
  // and persists via ConfigManager. OTA lives in the Config Manager tab
  // alongside every other config.
  addUpdateHandler([this](const String& origin) {
    requestConfigure();
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
  if (_reconfigure) {
    _reconfigure = false;  // before the work: a request meanwhile runs it again
    configureArduinoOTA();
  }
  if (_state.enabled && _arduinoOTA) {
    _arduinoOTA->handle();
  }
}

uint32_t OTASettingsService::loopWaitMs(uint32_t nowMs) const {
  (void)nowMs;
  if (_reconfigure) return 0;
  return (_state.enabled && _arduinoOTA) ? OTA_POLL_MS : ESPRACK_LOOP_IDLE_WAIT_MS;
}

void OTASettingsService::requestConfigure() {
  _reconfigure = true;
  ESPRack::wakeLoop();
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
    if (ESPRack::WriteGate::holders() > 0) {
      log_w("[ota] ArduinoOTA is on, but its writes don't go through the flash write gate (%u holders)",
            (unsigned)ESPRack::WriteGate::holders());
    }
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
      if (!takeUpdate()) {
        // Update.begin() already ran (nothing written yet): its abort ends
        // the transfer, onError(OTA_END_ERROR) follows and espota gets the
        // error.
        log_w("[ota] refused: another firmware update is under way (%s)", ESPRack::FirmwareUpdate::source());
#ifdef ESP32
        Update.abort();
#endif
        return;
      }
      s_otaOwn = true;
      Serial.println(F("Starting"));
      if (ESPRack::WriteGate::holders() > 0) {
        log_w("[ota] ArduinoOTA writes outside the flash write gate (%u holders): 64 KB erases may land in their windows",
              (unsigned)ESPRack::WriteGate::holders());
      }
      ESPRack::LoopWdt::pause();
    });
    // Reboot through the framework: the modules' onShutdown()
    // runs on the loop task and the next boot reports lastRestartReason()
    // "ota". ArduinoOTA has already answered OK when onEnd runs.
    _arduinoOTA->setRebootOnSuccess(false);
    _arduinoOTA->onEnd([]() {
      Serial.println(F("\r\nEnd"));
      giveUpdate(false);  // the loop watchdog stays paused up to the restart
      ESPRack::restartDevice(ESPRack::kRestartOta);
    });
    _arduinoOTA->onProgress([](unsigned int progress, unsigned int total) {
      if (s_otaOwn) ESPRack::FirmwareUpdate::progress();
      log_d("Progress: %u%%", (progress / (total / 100)));
    });
    _arduinoOTA->onError([](ota_error_t error) {
      giveUpdate(true);
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
  requestConfigure();
}
#endif
