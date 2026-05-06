#ifndef OTASettingsService_h
#define OTASettingsService_h

#include <HttpEndpoint.h>
#include <ConfigManager.h>
#include <ConfigDelegate.h>

#ifdef ESP32
#include <ESPmDNS.h>
#elif defined(ESP8266)
#include <ESP8266mDNS.h>
#endif

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

#define OTA_SETTINGS_FILE         "/config/otaSettings.json"
#define OTA_SETTINGS_SERVICE_PATH "/rest/otaSettings"
#define OTA_SETTINGS_FORM_PATH    "/rest/ota/settings"

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

  // ---- Legacy HttpEndpoint reader -- emits same shape as readConfig.
  // Kept distinct so callers that need to swap one without affecting
  // the other have an obvious seam.
  static void read(OTASettings& s, JsonObject& root) { readConfig(s, root); }

  // ---- Unified update: accepts either flat fields or the
  // DynamicSettings { settings: {...} } envelope. The legacy
  // HttpEndpoint and the form POST path both come through here.
  static StateUpdateResult update(JsonObject& root, OTASettings& s) {
    JsonObject src = root;
    if (root.containsKey("settings") && root["settings"].is<JsonObject>()) {
      src = root["settings"].as<JsonObject>();
    }
    OTASettings n = s;
    n.enabled  = src["enabled"]  | (bool)FACTORY_OTA_ENABLED;
    n.port     = src["port"]     | (int)FACTORY_OTA_PORT;
    n.password = src["pwd"] | String(FACTORY_OTA_PASSWORD);
    if (n.enabled == s.enabled && n.port == s.port && n.password == s.password) {
      return StateUpdateResult::UNCHANGED;
    }
    s = n;
    return StateUpdateResult::CHANGED;
  }

  // ---- Form schema (DynamicSettings UI) ---------------------------------
  static void buildForm(OTASettings& settings, JsonObject& root);
};

class WebManager;

class OTASettingsService : public StatefulService<OTASettings> {
 public:
  OTASettingsService(AsyncWebServer* server,
                     ConfigManager* cfgMgr,
                     SecurityManager* securityManager);

  // Contribute the 'ota' tab to the compound 'system' feature.
  void registerManifest(WebManager* web);

  void begin();
  void loop();

 private:
  HttpEndpoint<OTASettings>     _httpEndpoint;
  HttpEndpoint<OTASettings>     _formEndpoint;
  ConfigDelegate<OTASettings>   _cfg;
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
