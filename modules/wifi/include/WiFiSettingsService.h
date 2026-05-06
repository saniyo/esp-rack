#ifndef WiFiSettingsService_h
#define WiFiSettingsService_h

#include <SettingValue.h>
#include <StatefulService.h>
#include <ConfigManager.h>
#include <ConfigDelegate.h>
#include <FormBuilder.h>
#include <WebFeatureDelegate.h>
#include <JsonUtils.h>
#include <IPUtils.h>

#ifdef ESP32
#include <WiFi.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#endif

#ifndef FACTORY_WIFI_SSID
#define FACTORY_WIFI_SSID ""
#endif

#ifndef FACTORY_WIFI_PASSWORD
#define FACTORY_WIFI_PASSWORD ""
#endif

#ifndef FACTORY_WIFI_HOSTNAME
#define FACTORY_WIFI_HOSTNAME "#{platform}-#{unique_id}"
#endif

// Persisted config path — MUST stay identical to the pre-WebManager layout
// so firmware updates keep working without re-entering Wi-Fi credentials.
#define WIFI_SETTINGS_FILE "/config/wifiSettings.json"

// Unified REST form — one endpoint returns {status,scan,settings} envelope;
// each DynamicFeature tab extracts its own section via tab.key.
#define WIFI_FORM_PATH "/rest/wifiForm"

// Scan trigger action — user-pressed only, never auto-kicks.
#define WIFI_SCAN_KICK_PATH "/rest/wifiScan"

#define WIFI_RECONNECTION_DELAY (30UL * 1000UL)

// Dual-AP failover: after this many consecutive disconnect events on the
// currently-selected AP, swap to the other (if configured). Combined with
// the 30 s WIFI_RECONNECTION_DELAY cycle this gives ~60 s before fallover.
#define WIFI_AP_SWAP_AFTER_FAILS 2

class WebManager;

class WiFiSettings {
 public:
  // ---- Primary AP (field names match the legacy schema) ----
  String ssid;
  String password;

  // ---- Secondary AP (new, additive — legacy files missing these keys
  // load as empty strings so upgrades keep working).
  String ssid2;
  String password2;

  String hostname;
  bool staticIPConfig{false};

  IPAddress localIP;
  IPAddress gatewayIP;
  IPAddress subnetMask;
  IPAddress dnsIP1;
  IPAddress dnsIP2;

  bool operator==(const WiFiSettings& o) const {
    return ssid == o.ssid && password == o.password &&
           ssid2 == o.ssid2 && password2 == o.password2 &&
           hostname == o.hostname && staticIPConfig == o.staticIPConfig &&
           localIP == o.localIP && gatewayIP == o.gatewayIP && subnetMask == o.subnetMask &&
           dnsIP1 == o.dnsIP1 && dnsIP2 == o.dnsIP2;
  }

  // --- Persistence (written to /config/wifiSettings.json) ---
  // Keys are stable: any addition here is *additive* so old saved files
  // still load (unknown keys ignored by update(); missing keys default).
  static void readConfig(WiFiSettings& s, JsonObject& root) {
    root["ssid"] = s.ssid;
    root["pwd"] = s.password;
    root["ssid_2"] = s.ssid2;
    root["pwd_2"] = s.password2;
    root["hostname"] = s.hostname;
    root["static_ip_config"] = s.staticIPConfig;
    JsonUtils::writeIP(root, "local_ip", s.localIP);
    JsonUtils::writeIP(root, "gateway_ip", s.gatewayIP);
    JsonUtils::writeIP(root, "subnet_mask", s.subnetMask);
    JsonUtils::writeIP(root, "dns_ip_1", s.dnsIP1);
    JsonUtils::writeIP(root, "dns_ip_2", s.dnsIP2);
  }

  // --- REST form (Status tab + Scan tab + Settings tab) ---
  static void buildForm(WiFiSettings& s, JsonObject& root);

  // --- Unified POST handler — accepts either the raw settings dict or
  // a DynamicSettings envelope { "settings": {...} }.
  static StateUpdateResult update(JsonObject& root, WiFiSettings& s);
};

class WiFiSettingsService : public StatefulService<WiFiSettings> {
 public:
  explicit WiFiSettingsService(ConfigManager* cfgMgr);

  // Declares the feature in /rest/uiManifest and mounts the form + scan action.
  void registerManifest(WebManager* web);

  void begin();
  void loop();

  // Dual-AP active slot, exposed read-only for buildForm's Status tab.
  // 0 = primary, 1 = secondary.
  uint8_t activeAp() const { return _activeAp; }

  // Scan coordination — guard for manageSTA() so the radio isn't re-begin'd
  // while a scan is in flight. Auto-expires after 30 s as safety net.
  void setScanInProgress(bool v) {
    _scanInProgress = v;
    if (v) _scanStartedAt = millis();
  }
  bool isScanInProgress() const {
    if (!_scanInProgress) return false;
    if ((unsigned long)(millis() - _scanStartedAt) > 30000UL) return false;
    return true;
  }

 private:
  ConfigDelegate<WiFiSettings> _cfg;
  WebFeatureEntry<WiFiSettings>* _feature{nullptr};

  unsigned long _lastConnectionAttempt{0};

  bool _scanInProgress{false};
  unsigned long _scanStartedAt{0};

  // Dual-AP state machine
  uint8_t _activeAp{0};      // 0 = primary, 1 = secondary
  uint8_t _apAttempts{0};    // consecutive disconnects on the current AP

  // POST update handlers run on the async_tcp task; FS writes (saveIfChanged)
  // and WiFi.disconnect() block that task, delaying the response flush and
  // making the Save button look frozen for hundreds of ms. We defer both
  // to main-loop task via these flags — the HTTP handler returns instantly,
  // the browser sees the 200 immediately, and the heavy work happens next
  // iteration of loop().
  volatile bool _pendingSave{false};
  volatile bool _pendingReconfigure{false};
  String        _pendingSaveOrigin;

#ifdef ESP32
  bool _stopping{false};
  void onStationModeConnected(WiFiEvent_t event, WiFiEventInfo_t info);
  void onStationModeDisconnected(WiFiEvent_t event, WiFiEventInfo_t info);
  void onStationModeGotIP(WiFiEvent_t event, WiFiEventInfo_t info);
  void onStationModeStop(WiFiEvent_t event, WiFiEventInfo_t info);
#endif

  void reconfigureWiFiConnection();
  void manageSTA();
  void ensureStaEnabledNoKillAp();

  void kickScan(AsyncWebServerRequest* request);

  // Pick credentials for the currently active AP slot, falling back to the
  // other slot if the selected one is empty.
  void pickActiveCreds(String& outSsid, String& outPass);
};

#endif
