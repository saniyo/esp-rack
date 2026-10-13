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

 private:
  ConfigDelegate<OTASettings>   _cfg;
  WebFeatureEntry<OTASettings>* _feature{nullptr};
  ArduinoOTAClass*              _arduinoOTA{nullptr};

  void configureArduinoOTA();
#ifdef ESP32
  void onStationModeGotIP(WiFiEvent_t event, WiFiEventInfo_t info);
#elif defined(ESP8266)
  WiFiEventHandler _onStationModeGotIPHandler;
  void onStationModeGotIP(const WiFiEventStationModeGotIP& event);
#endif
};

#endif  // end OTASettingsService_h
