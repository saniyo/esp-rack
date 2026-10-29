#ifndef OTASettingsService_h
#define OTASettingsService_h

#include <StatefulService.h>
#include <ConfigManager.h>
#include <ConfigDelegate.h>
#include <FormBuilder.h>
#include <WebFeatureDelegate.h>

// WiFiEvent_t / WiFiEventInfo_t for the onStationMode* handlers below.
// ESPAsyncWebServer <= 3.6 pulled <WiFi.h> in transitively; ESP32Async
// 3.7+ is leaner about its includes, so we depend on it explicitly.
#include <WiFi.h>

#include <ArduinoOTA.h>
#include <WiFiUdp.h>

#ifndef FACTORY_OTA_PORT
#define FACTORY_OTA_PORT 8266
#endif

#ifndef FACTORY_OTA_PASSWORD
#define FACTORY_OTA_PASSWORD "esp-react"
#endif

#ifndef FACTORY_OTA_ENABLED
#define FACTORY_OTA_ENABLED true
#endif

#define OTA_SETTINGS_FILE "/config/otaSettings.json"

// How often loop() asks ArduinoOTA for an invitation. Exception to "no
// polling": ArduinoOTA keeps its UDP socket private and has no callback, so
// handle() is the only way in; espota waits seconds for the answer.
#ifndef OTA_POLL_MS
#define OTA_POLL_MS 100
#endif

class OTASettings {
 public:
  bool   enabled{FACTORY_OTA_ENABLED};
  int    port{FACTORY_OTA_PORT};
  String password{FACTORY_OTA_PASSWORD};

  // ---- ConfigDelegate persistence (full state in/out) -------------------
  // ConfigManager calls this both as the reader (on save → atomicWrite)
  // and the JSON shape it expects on load (deserialize → updater).
  // Keys MUST stay stable so existing /config/otaSettings.json files
  // from the FSPersistence era keep loading.
  static void readConfig(OTASettings& s, JsonObject& root) {
    root["enabled"]  = s.enabled;
    root["port"]     = s.port;
    root["pwd"] = s.password;
  }

  // ---- Unified update: accepts either flat fields or the
  // DynamicSettings { settings: {...} } envelope.
  static StateUpdateResult update(JsonObject& root, OTASettings& s) {
    JsonObject src = root;
    if (root.containsKey("settings") && root["settings"].is<JsonObject>()) {
      src = root["settings"].as<JsonObject>();
    }
    // Absent keys leave the field unchanged: the UI saves only the keys
    // the operator edited, so a "{enabled:false}" save must not reset the
    // port and password to factory. Defaults come from the member
    // initialisers (first boot / resetToDefaults), not from here.
    bool changed = false;
    changed |= FormBuilder::updateValue(src, "enabled", s.enabled);
    changed |= FormBuilder::updateValue(src, "port", s.port);
    changed |= FormBuilder::updateValue(src, "pwd", s.password);
    return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
  }

  // ---- Form schema (DynamicSettings UI) ---------------------------------
  static void buildForm(OTASettings& settings, JsonObject& root);
};

class WebManager;

// ArduinoOTA (espota / PlatformIO network upload) and the flash write gate
// (power/WriteGate.h): ArduinoOTA writes the image through Arduino's
// Update, which erases a whole 64 KB block at every 64 KB boundary (the
// CPU caches are off for ~0.15-1 s each time) and blocks the loop task for
// the whole transfer. That can't wait for the gate's pauses, so it does
// NOT go through the gate: on a device where a module holds write windows
// (WriteGate::holders() > 0) an ArduinoOTA upload can land in them. The
// page says so and the start is logged as a warning; the upload page and
// auto-update go through the gate (ESPRack::OtaWriter). Use those on such
// a device, or switch ArduinoOTA off there.
class OTASettingsService : public StatefulService<OTASettings> {
 public:
  // OTA is fully mDNS-free: ArduinoOTA's own responder is disabled and the
  // framework ships no mDNS module. Upload by IP, or <host>.local via lwIP's
  // native one-shot mDNS query (no responder needed).
  OTASettingsService(ConfigManager* cfgMgr);

  // Contribute the 'ota' tab to the compound 'system' feature.
  void registerManifest(WebManager* web);

  void begin();
  void loop();
  // 0 for a pending reconfigure, OTA_POLL_MS while ArduinoOTA listens, idle
  // otherwise.
  uint32_t loopWaitMs(uint32_t nowMs) const;

 private:
  ConfigDelegate<OTASettings>   _cfg;
  WebFeatureEntry<OTASettings>* _feature{nullptr};
  ArduinoOTAClass*              _arduinoOTA{nullptr};
  // GOT_IP (arduino_events) and the settings handler (async_tcp) only ask;
  // loop() rebuilds ArduinoOTA, so handle() never runs on a deleted one.
  volatile bool                 _reconfigure{false};
  void requestConfigure();

  void configureArduinoOTA();
#ifdef ESP32
  void onStationModeGotIP(WiFiEvent_t event, WiFiEventInfo_t info);
#elif defined(ESP8266)
  WiFiEventHandler _onStationModeGotIPHandler;
  void onStationModeGotIP(const WiFiEventStationModeGotIP& event);
#endif
};

#endif  // end OTASettingsService_h
