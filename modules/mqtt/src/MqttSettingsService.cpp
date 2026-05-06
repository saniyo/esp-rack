#include <MqttSettingsService.h>
#include <WebManager.h>

static char* retainCstr(const char* cstr, char** ptr) {
  free(*ptr);
  *ptr = nullptr;
  if (cstr != nullptr) {
    *ptr = (char*)malloc(strlen(cstr) + 1);
    strcpy(*ptr, cstr);
  }
  return *ptr;
}

/* ---------- ctor ---------- */
MqttSettingsService::MqttSettingsService(ConfigManager* cfgMgr)
    : StatefulService<MqttSettings>(),
      _cfg(cfgMgr,
           "mqtt",
           MQTT_FILE,
           4096,
           this,
           MqttSettings::readConfig,
           MqttSettings::update,
           false /*autoSave*/,
           nullptr /*validator*/,
           MqttSettings::buildForm /*formReader*/),
      _mqttClient() {
#ifdef ESP32
  WiFi.onEvent(
      std::bind(&MqttSettingsService::onStationModeDisconnected, this, std::placeholders::_1, std::placeholders::_2),
      WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent(std::bind(&MqttSettingsService::onStationModeGotIP, this, std::placeholders::_1, std::placeholders::_2),
               WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
#elif defined(ESP8266)
  _onStationModeDisconnectedHandler = WiFi.onStationModeDisconnected(
      std::bind(&MqttSettingsService::onStationModeDisconnected, this, std::placeholders::_1));
  _onStationModeGotIPHandler =
      WiFi.onStationModeGotIP(std::bind(&MqttSettingsService::onStationModeGotIP, this, std::placeholders::_1));
#endif

  _mqttClient.onConnect(std::bind(&MqttSettingsService::onMqttConnect, this, std::placeholders::_1));
  _mqttClient.onDisconnect(std::bind(&MqttSettingsService::onMqttDisconnect, this, std::placeholders::_1));
  _mqttClient.onMessage(std::bind(&MqttSettingsService::onMqttMessage,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3,
                                  std::placeholders::_4,
                                  std::placeholders::_5,
                                  std::placeholders::_6));

  addUpdateHandler([this](const String& origin) {
    onConfigUpdated();
    _cfg.saveIfChanged(origin);
  }, false);
}

MqttSettingsService::~MqttSettingsService() {
}

/* ---------- registerManifest ---------- */
void MqttSettingsService::registerManifest(WebManager* web) {
  if (!web) return;

  WebFeatureSpec spec;
  spec.id         = "mqtt";
  spec.title      = "MQTT";
  spec.component  = "DynamicSettings";
  spec.menu.label = "MQTT";
  spec.menu.icon  = "DeviceHub";
  spec.menu.order = 350;
  spec.menu.auth  = WebAuthLevel::Authenticated;
  spec.auth       = WebAuthLevel::Authenticated;
  spec.restRead   = MQTT_FORM_PATH;
  spec.restUpdate = MQTT_FORM_PATH;
  spec.wsPath     = MQTT_WS_PATH;

  WebTabSpec statusTab;
  statusTab.key      = "status";
  statusTab.title    = "Status";
  statusTab.restPath = MQTT_FORM_PATH;
  statusTab.postable = false;
  statusTab.live     = true;
  statusTab.auth     = WebAuthLevel::Authenticated;
  spec.tabs.push_back(statusTab);

  WebTabSpec settingsTab;
  settingsTab.key      = "settings";
  settingsTab.title    = "Settings";
  settingsTab.restPath = MQTT_FORM_PATH;
  settingsTab.postable = true;
  settingsTab.auth     = WebAuthLevel::Admin;
  spec.tabs.push_back(settingsTab);

  _feature = web->registerFeature<MqttSettings>(
      std::move(spec), this,
      MqttSettings::buildForm, MqttSettings::update,     // REST
      MqttSettings::staRead,   MqttSettings::staUpd,     // WS
      4096, 4096);
}

/* ---------- begin ---------- */
void MqttSettingsService::begin() {
  (void)_cfg.ensureLoaded();

  // Client-id placeholder expansion only if user left the default/empty.
  if (_state.clientId.length() == 0) {
    _state.clientId = SettingValue::format(FACTORY_MQTT_CLIENT_ID);
  }

  if (_state.enabled) {
    Serial.println(F("MQTT is enabled, configuring..."));
    configureMqtt();
  }

  refreshRuntime();
  if (_feature) _feature->broadcastWs("boot");
}

/* ---------- loop ---------- */
void MqttSettingsService::loop() {
  if (_reconfigureMqtt ||
      (_disconnectedAt && (unsigned long)(millis() - _disconnectedAt) >= MQTT_RECONNECTION_DELAY)) {
    configureMqtt();
    _reconfigureMqtt = false;
    _disconnectedAt = 0;
  }
}

/* ---------- legacy accessors ---------- */
bool MqttSettingsService::isEnabled() {
  return _state.enabled;
}

bool MqttSettingsService::isConnected() {
  return _mqttClient.connected();
}

const char* MqttSettingsService::getClientId() {
  return _mqttClient.getClientId();
}

AsyncMqttClientDisconnectReason MqttSettingsService::getDisconnectReason() {
  return (AsyncMqttClientDisconnectReason)_state.disconnectReason;
}

AsyncMqttClient* MqttSettingsService::getMqttClient() {
  return &_mqttClient;
}

/* ---------- mqtt callbacks ---------- */
void MqttSettingsService::onMqttConnect(bool sessionPresent) {
  Serial.print(F("Connected to MQTT, "));
  Serial.println(sessionPresent ? F("with persistent session") : F("without persistent session"));

  refreshRuntime();
  if (_feature) _feature->broadcastWs("mqtt");

  for (auto& cb : _connectListeners) cb(sessionPresent);
}

void MqttSettingsService::onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.print(F("Disconnected from MQTT reason: "));
  Serial.println((uint8_t)reason);

  _state.disconnectReason = (uint8_t)reason;
  _disconnectedAt = millis();

  refreshRuntime();
  if (_feature) _feature->broadcastWs("mqtt");

  for (auto& cb : _disconnectListeners) cb(reason);
}

void MqttSettingsService::onMqttMessage(char* topic,
                                        char* payload,
                                        AsyncMqttClientMessageProperties properties,
                                        size_t len,
                                        size_t index,
                                        size_t total) {
  if (topic) {
    String t(topic);
    if (_discoveredTopics.insert(t).second) {
      appendTopic(t);
      // Only push the topic-list refresh if someone is actually viewing the
      // MQTT status tab — busy brokers would otherwise fire a WS serialise
      // on every inbound message and hammer the main loop.
      if (_feature && _feature->hasSubscribers()) _feature->broadcastWs("mqtt");
    }
  }
  for (auto& cb : _msgListeners) cb(topic, payload, properties, len, index, total);
}

/* ---------- IMqttDispatcher ---------- */
void MqttSettingsService::addMessageCallback(MqttMsgCb cb)       { _msgListeners.push_back(cb); }
void MqttSettingsService::addConnectCallback(MqttConnectCb cb)   { _connectListeners.push_back(cb); }
void MqttSettingsService::addDisconnectCallback(MqttDisconnectCb cb) { _disconnectListeners.push_back(cb); }

void MqttSettingsService::onConfigUpdated() {
  _reconfigureMqtt = true;
  _disconnectedAt = 0;
}

#ifdef ESP32
void MqttSettingsService::onStationModeGotIP(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (_state.enabled) {
    Serial.println(F("WiFi connected, starting MQTT client."));
    onConfigUpdated();
  }
}
void MqttSettingsService::onStationModeDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (_state.enabled) {
    Serial.println(F("WiFi dropped, stopping MQTT client."));
    onConfigUpdated();
  }
}
#elif defined(ESP8266)
void MqttSettingsService::onStationModeGotIP(const WiFiEventStationModeGotIP& event) {
  if (_state.enabled) onConfigUpdated();
}
void MqttSettingsService::onStationModeDisconnected(const WiFiEventStationModeDisconnected& event) {
  if (_state.enabled) onConfigUpdated();
}
#endif

/* ---------- configureMqtt ---------- */
void MqttSettingsService::configureMqtt() {
  _mqttClient.disconnect();

  if (_state.enabled && WiFi.isConnected()) {
    Serial.println(F("Connecting to MQTT..."));
    _mqttClient.setServer(retainCstr(_state.host.c_str(), &_retainedHost), _state.port);

    if (_state.username.length() > 0) {
      _mqttClient.setCredentials(
          retainCstr(_state.username.c_str(), &_retainedUsername),
          retainCstr(_state.password.length() > 0 ? _state.password.c_str() : nullptr, &_retainedPassword));
    } else {
      _mqttClient.setCredentials(retainCstr(nullptr, &_retainedUsername),
                                 retainCstr(nullptr, &_retainedPassword));
    }

    _mqttClient.setClientId(retainCstr(_state.clientId.c_str(), &_retainedClientId));
    _mqttClient.setKeepAlive(_state.keepAlive);
    _mqttClient.setCleanSession(_state.cleanSession);
    _mqttClient.setMaxTopicLength(_state.maxTopicLength);
    _mqttClient.connect();
  }

  refreshRuntime();
  if (_feature) _feature->broadcastWs("mqtt");
}

/* ---------- runtime mirrors ---------- */
void MqttSettingsService::refreshRuntime() {
  _state.connected = _mqttClient.connected();
  const char* cid = _mqttClient.getClientId();
  _state.runtimeClientId = cid ? cid : "";
}

void MqttSettingsService::appendTopic(const String& topic) {
  // Keep a rolling newline-joined list of the most recent MQTT_TOPICS_LOG_MAX
  // topics. Renders naturally into the multi-line "topics" text field.
  if (_discoveredTopics.size() > MQTT_TOPICS_LOG_MAX) {
    // trim oldest by rebuilding from the set keyed by insertion order isn't
    // available (std::set is sorted); instead just cap the joined string size.
    _discoveredTopics.erase(_discoveredTopics.begin());
  }

  String joined;
  joined.reserve(_discoveredTopics.size() * 32);
  bool first = true;
  for (const auto& t : _discoveredTopics) {
    if (!first) joined += '\n';
    joined += t;
    first = false;
  }
  _state.topicsJoined = joined;
}
