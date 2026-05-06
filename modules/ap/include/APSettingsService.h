#pragma once
#ifndef APSettingsConfig_h
#define APSettingsConfig_h

#include <StatefulService.h>
#include <ConfigManager.h>
#include <ConfigDelegate.h>
#include <FormBuilder.h>
#include <WebFeatureDelegate.h>

#include <SettingValue.h>
#include <JsonUtils.h>

#include <DNSServer.h>
#include <IPAddress.h>
#ifdef ESP32
#include <WiFi.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#endif

#ifndef FACTORY_AP_PROVISION_MODE
#define FACTORY_AP_PROVISION_MODE AP_MODE_ALWAYS
#endif
#ifndef FACTORY_AP_SSID
#define FACTORY_AP_SSID "ESP-React-#{unique_id}"
#endif
#ifndef FACTORY_AP_PASSWORD
#define FACTORY_AP_PASSWORD "esp-react"
#endif
#ifndef FACTORY_AP_LOCAL_IP
#define FACTORY_AP_LOCAL_IP "192.168.4.1"
#endif
#ifndef FACTORY_AP_GATEWAY_IP
#define FACTORY_AP_GATEWAY_IP "192.168.4.1"
#endif
#ifndef FACTORY_AP_SUBNET_MASK
#define FACTORY_AP_SUBNET_MASK "255.255.255.0"
#endif
#ifndef FACTORY_AP_CHANNEL
#define FACTORY_AP_CHANNEL 1
#endif
#ifndef FACTORY_AP_SSID_HIDDEN
#define FACTORY_AP_SSID_HIDDEN false
#endif
#ifndef FACTORY_AP_MAX_CLIENTS
#define FACTORY_AP_MAX_CLIENTS 4
#endif

#define AP_FILE       "/config/apSettings.json"
#define AP_FORM_PATH  "/rest/apForm"

#define AP_MODE_ALWAYS 0
#define AP_MODE_DISCONNECTED 1
#define AP_MODE_NEVER 2

#define MANAGE_NETWORK_DELAY 10000
#define DNS_PORT 53

enum APNetworkStatus { ACTIVE = 0, INACTIVE, LINGERING };

class WebManager;

/* ================= SETTINGS ================= */
class APSettings {
 public:
  // persisted config
  uint8_t provisionMode = FACTORY_AP_PROVISION_MODE;
  String ssid;  // resolved in APSettingsService::begin via SettingValue::format
  String password = FACTORY_AP_PASSWORD;
  uint8_t channel = FACTORY_AP_CHANNEL;
  bool ssidHidden = FACTORY_AP_SSID_HIDDEN;
  uint8_t maxClients = FACTORY_AP_MAX_CLIENTS;
  IPAddress localIP;
  IPAddress gatewayIP;
  IPAddress subnetMask;

  bool operator==(const APSettings& o) const {
    return provisionMode == o.provisionMode && ssid == o.ssid && password == o.password &&
           channel == o.channel && ssidHidden == o.ssidHidden && maxClients == o.maxClients &&
           localIP == o.localIP && gatewayIP == o.gatewayIP && subnetMask == o.subnetMask;
  }

  static const char* statusLabel(uint8_t st) {
    switch ((APNetworkStatus)st) {
      case APNetworkStatus::ACTIVE:    return "Active";
      case APNetworkStatus::INACTIVE:  return "Inactive";
      case APNetworkStatus::LINGERING: return "Lingering until idle";
    }
    return "Unknown";
  }

  static const char* statusColor(uint8_t st) {
    switch ((APNetworkStatus)st) {
      case APNetworkStatus::ACTIVE:    return "success";
      case APNetworkStatus::INACTIVE:  return "info";
      case APNetworkStatus::LINGERING: return "warning";
    }
    return "warning";
  }

  /* ----- CONFIG persistence ----- */
  static void readConfig(APSettings& s, JsonObject& root) {
    root["provision_mode"] = s.provisionMode;
    root["ssid"] = s.ssid;
    root["pwd"] = s.password;
    root["channel"] = s.channel;
    root["ssid_hidden"] = s.ssidHidden;
    root["max_clients"] = s.maxClients;
    root["local_ip"] = s.localIP.toString();
    root["gateway_ip"] = s.gatewayIP.toString();
    root["subnet_mask"] = s.subnetMask.toString();
  }

  /* ----- REST form (status + settings) ----- */
  static void buildForm(APSettings& s, JsonObject& root);

  /* ----- Unified update ----- */
  static StateUpdateResult update(JsonObject& root, APSettings& s);
};

/* ================= SERVICE ================= */
class APSettingsService : public StatefulService<APSettings> {
 public:
  APSettingsService(ConfigManager* cfgMgr);

  void registerManifest(WebManager* web);

  void begin();
  void loop();

  APNetworkStatus getAPNetworkStatus();

 private:
  ConfigDelegate<APSettings> _cfg;
  WebFeatureEntry<APSettings>* _feature{nullptr};

  DNSServer* _dnsServer{nullptr};
  volatile unsigned long _lastManaged{0};
  volatile bool _reconfigureAp{false};

  void reconfigureAP();
  void manageAP();
  void startAP();
  void stopAP();
  void handleDNS();
};

#endif  // APSettingsConfig_h
