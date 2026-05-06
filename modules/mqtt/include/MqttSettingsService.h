#pragma once
#ifndef MqttSettingsService_h
#define MqttSettingsService_h

#include <StatefulService.h>
#include <ConfigManager.h>
#include <ConfigDelegate.h>
#include <FormBuilder.h>
#include <WebFeatureDelegate.h>
#include <AsyncMqttClient.h>
#include <SettingValue.h>

#include "IMqttDispatcher.h"
#include <vector>
#include <set>

#ifndef FACTORY_MQTT_ENABLED
#define FACTORY_MQTT_ENABLED false
#endif
#ifndef FACTORY_MQTT_HOST
#define FACTORY_MQTT_HOST "test.mosquitto.org"
#endif
#ifndef FACTORY_MQTT_PORT
#define FACTORY_MQTT_PORT 1883
#endif
#ifndef FACTORY_MQTT_USERNAME
#define FACTORY_MQTT_USERNAME ""
#endif
#ifndef FACTORY_MQTT_PASSWORD
#define FACTORY_MQTT_PASSWORD ""
#endif
#ifndef FACTORY_MQTT_CLIENT_ID
#define FACTORY_MQTT_CLIENT_ID "#{platform}-#{unique_id}"
#endif
#ifndef FACTORY_MQTT_KEEP_ALIVE
#define FACTORY_MQTT_KEEP_ALIVE 16
#endif
#ifndef FACTORY_MQTT_CLEAN_SESSION
#define FACTORY_MQTT_CLEAN_SESSION true
#endif
#ifndef FACTORY_MQTT_MAX_TOPIC_LENGTH
#define FACTORY_MQTT_MAX_TOPIC_LENGTH 128
#endif

#define MQTT_FILE         "/config/mqttSettings.json"
#define MQTT_FORM_PATH    "/rest/mqttForm"
#define MQTT_WS_PATH      "/ws/mqttStatus"

#define MQTT_RECONNECTION_DELAY 5000
#define MQTT_TOPICS_LOG_MAX     20

class WebManager;

/* ================= SETTINGS ================= */
class MqttSettings {
 public:
  // persisted config
  bool enabled = FACTORY_MQTT_ENABLED;
  String host = FACTORY_MQTT_HOST;
  uint16_t port = FACTORY_MQTT_PORT;
  String username = FACTORY_MQTT_USERNAME;
  String password = FACTORY_MQTT_PASSWORD;
  String clientId;  // filled on begin() via SettingValue::format if empty
  uint16_t keepAlive = FACTORY_MQTT_KEEP_ALIVE;
  bool cleanSession = FACTORY_MQTT_CLEAN_SESSION;
  uint16_t maxTopicLength = FACTORY_MQTT_MAX_TOPIC_LENGTH;

  // runtime (not persisted)
  bool connected{false};
  String runtimeClientId;
  uint8_t disconnectReason{0};
  String topicsJoined;  // newline-separated rolling log

  static const char* reasonText(uint8_t r) {
    switch ((AsyncMqttClientDisconnectReason)r) {
      case AsyncMqttClientDisconnectReason::TCP_DISCONNECTED: return "TCP disconnected";
      case AsyncMqttClientDisconnectReason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION: return "Protocol rejected";
      case AsyncMqttClientDisconnectReason::MQTT_IDENTIFIER_REJECTED: return "Client ID rejected";
      case AsyncMqttClientDisconnectReason::MQTT_SERVER_UNAVAILABLE: return "Server unavailable";
      case AsyncMqttClientDisconnectReason::MQTT_MALFORMED_CREDENTIALS: return "Malformed credentials";
      case AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED: return "Not authorized";
      case AsyncMqttClientDisconnectReason::ESP8266_NOT_ENOUGH_SPACE: return "Out of memory";
      case AsyncMqttClientDisconnectReason::TLS_BAD_FINGERPRINT: return "TLS fingerprint invalid";
      default: return "Unknown";
    }
  }

  static String statusLabel(const MqttSettings& s) {
    if (!s.enabled) return String("Not enabled");
    if (s.connected) return String("Connected");
    return String("Disconnected (") + reasonText(s.disconnectReason) + ")";
  }

  /* ----- CONFIG persistence (ConfigDelegate) ----- */
  static void readConfig(MqttSettings& s, JsonObject& root) {
    root["enabled"] = s.enabled;
    root["host"] = s.host;
    root["port"] = s.port;
    root["username"] = s.username;
    root["pwd"] = s.password;
    root["client_id"] = s.clientId;
    root["keep_alive"] = s.keepAlive;
    root["clean_session"] = s.cleanSession;
    root["max_topic_length"] = s.maxTopicLength;
  }

  /* ----- WS status reader (runtime + config echo) ----- */
  static void staRead(MqttSettings& s, JsonObject& root) {
    root["status"] = statusLabel(s);
    root["enabled"] = s.enabled;
    root["connected"] = s.connected;
    root["client_id"] = s.runtimeClientId;
    root["disconnect_reason"] = s.disconnectReason;
    root["topics"] = s.topicsJoined;

    // echo config so the settings tab stays in sync when another session edits
    root["host"] = s.host;
    root["port"] = s.port;
    root["username"] = s.username;
    root["pwd"] = s.password;
    root["client_id_cfg"] = s.clientId;
    root["keep_alive"] = s.keepAlive;
    root["clean_session"] = s.cleanSession;
    root["max_topic_length"] = s.maxTopicLength;
  }

  static StateUpdateResult staUpd(JsonObject& in, MqttSettings& s) {
    return update(in, s);
  }

  /* ----- REST form (status + settings) ----- */
  static void buildForm(MqttSettings& s, JsonObject& root) {
    // STATUS
    JsonArray st = FormBuilder::createForm(root, "status", "Status");
    FormBuilder::addTextField(st, "status", AF::R, statusLabel(s).c_str(),
                              label("Status"), icon("DeviceHub"));
    FormBuilder::addSwitchField(st, "connected", AF::R, s.connected,
                                label("Connected"));
    FormBuilder::addTextField(st, "client_id", AF::R, s.runtimeClientId.c_str(),
                              label("Client ID"), icon("Memory"));
    FormBuilder::addTextField(st, "topics", AF::R, s.topicsJoined.c_str(),
                              label("Topics"), icon("Topic"));

    // SETTINGS
    JsonArray set = FormBuilder::createForm(root, "settings", "Settings");
    FormBuilder::addSwitchField(set, "enabled", AF::RW, s.enabled,
                                label("Enabled"), icon("Sync"));
    FormBuilder::addTextField(set, "host", AF::RW, s.host.c_str(),
                              label("Host"), icon("Cloud"));
    FormBuilder::addNumberField(set, "port", AF::RW, s.port,
                                label("Port"),
                                minVal(1), maxVal(65535), format("0"));
    FormBuilder::addTextField(set, "username", AF::RW, s.username.c_str(),
                              label("Username"), icon("Lock"));
    FormBuilder::addSecretField(set, "pwd", AF::RW, s.password.c_str(),
                                label("Password"), icon("Lock"));
    FormBuilder::addTextField(set, "client_id_cfg", AF::RW, s.clientId.c_str(),
                              label("Client ID"), icon("Memory"));
    FormBuilder::addNumberField(set, "keep_alive", AF::RW, s.keepAlive,
                                label("Keep alive"),
                                minVal(1), maxVal(600), format("0"), unit("s"), icon("Timer"));
    FormBuilder::addSwitchField(set, "clean_session", AF::RW, s.cleanSession,
                                label("Clean session"));
    FormBuilder::addNumberField(set, "max_topic_length", AF::RW, s.maxTopicLength,
                                label("Max topic length"),
                                minVal(16), maxVal(4096), format("0"), unit("B"));
  }

  /* ----- Unified update (REST + FS + WS) ----- */
  static StateUpdateResult update(JsonObject& root, MqttSettings& s) {
    JsonObject src = root;
    if (root.containsKey("settings") && root["settings"].is<JsonObject>()) {
      src = root["settings"].as<JsonObject>();
    }

    bool changed = false;
    changed |= FormBuilder::updateValue(src, "enabled", s.enabled);
    changed |= FormBuilder::updateValue(src, "host", s.host);
    changed |= FormBuilder::updateValue(src, "port", s.port);
    changed |= FormBuilder::updateValue(src, "username", s.username);
    changed |= FormBuilder::updateValue(src, "pwd", s.password);
    // both persistence key ("client_id") and settings-tab key ("client_id_cfg") map to clientId
    changed |= FormBuilder::updateValue(src, "client_id", s.clientId);
    changed |= FormBuilder::updateValue(src, "client_id_cfg", s.clientId);
    changed |= FormBuilder::updateValue(src, "keep_alive", s.keepAlive);
    changed |= FormBuilder::updateValue(src, "clean_session", s.cleanSession);
    changed |= FormBuilder::updateValue(src, "max_topic_length", s.maxTopicLength);

    return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
  }
};

/* ================= SERVICE ================= */
class MqttSettingsService : public StatefulService<MqttSettings>, public IMqttDispatcher {
 public:
  MqttSettingsService(ConfigManager* cfgMgr);
  ~MqttSettingsService();

  // Publish the 'mqtt' feature (REST + WS + manifest metadata) via WebManager.
  void registerManifest(WebManager* web);

  void begin();
  void loop();

  // Legacy API retained for other services.
  bool isEnabled();
  bool isConnected();
  const char* getClientId();
  AsyncMqttClientDisconnectReason getDisconnectReason();
  AsyncMqttClient* getMqttClient() override;

  // IMqttDispatcher
  void addMessageCallback(MqttMsgCb cb) override;
  void addConnectCallback(MqttConnectCb cb) override;
  void addDisconnectCallback(MqttDisconnectCb cb) override;

 protected:
  void onConfigUpdated();

 private:
  ConfigDelegate<MqttSettings> _cfg;
  WebFeatureEntry<MqttSettings>* _feature{nullptr};

  // Listeners
  std::vector<MqttMsgCb> _msgListeners;
  std::vector<MqttConnectCb> _connectListeners;
  std::vector<MqttDisconnectCb> _disconnectListeners;

  // Retained cstr buffers for AsyncMqttClient (it keeps refs to these).
  char* _retainedHost{nullptr};
  char* _retainedClientId{nullptr};
  char* _retainedUsername{nullptr};
  char* _retainedPassword{nullptr};

  bool _reconfigureMqtt{false};
  unsigned long _disconnectedAt{0};

  AsyncMqttClient _mqttClient;

  std::set<String> _discoveredTopics;

#ifdef ESP32
  void onStationModeGotIP(WiFiEvent_t event, WiFiEventInfo_t info);
  void onStationModeDisconnected(WiFiEvent_t event, WiFiEventInfo_t info);
#elif defined(ESP8266)
  WiFiEventHandler _onStationModeDisconnectedHandler;
  WiFiEventHandler _onStationModeGotIPHandler;
  void onStationModeGotIP(const WiFiEventStationModeGotIP& event);
  void onStationModeDisconnected(const WiFiEventStationModeDisconnected& event);
#endif

  void onMqttConnect(bool sessionPresent);
  void onMqttDisconnect(AsyncMqttClientDisconnectReason reason);
  void onMqttMessage(char* topic,
                     char* payload,
                     AsyncMqttClientMessageProperties properties,
                     size_t len,
                     size_t index,
                     size_t total);
  void configureMqtt();
  void refreshRuntime();
  void appendTopic(const String& topic);
};

#endif  // MqttSettingsService_h
