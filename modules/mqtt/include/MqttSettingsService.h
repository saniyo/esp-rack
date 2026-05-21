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
#include "IMqttProvider.h"
#include "MqttSubscription.h"
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
// clientId is NOT operator-configurable. It is computed at every
// connect from DeviceIdentity::canonical() so it stays in lock-step
// with the cert CN / mothership deviceId / AutoUpdate did= — one
// source of truth for the device's on-the-wire identity. The
// FACTORY_MQTT_CLIENT_ID macro is intentionally absent; any value an
// older /config/mqttSettings.json may carry under "client_id" is
// ignored on load and overwritten on the next save.
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
#define MQTT_TOPICS_LOG_MAX     50
#define MQTT_SEEN_PAYLOAD_MAX   48     // truncate per-topic last-payload to this many chars
#define MQTT_ATTACHED_TOPICS_MAX 16    // upper bound on persisted attached topics

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
  // clientId is RUNTIME-ONLY — overwritten on every begin() with
  // DeviceIdentity::canonical(). Not persisted, not exposed in the
  // form. Lives in the struct purely so AsyncMqttClient::setClientId
  // can take the c_str(); see configureMqtt() in the .cpp.
  String clientId;
  uint16_t keepAlive = FACTORY_MQTT_KEEP_ALIVE;
  bool cleanSession = FACTORY_MQTT_CLEAN_SESSION;
  uint16_t maxTopicLength = FACTORY_MQTT_MAX_TOPIC_LENGTH;

  // Persisted: list of topic patterns the user has "attached" — these
  // get re-subscribed on every (re)connect so the user's discovery
  // choices survive reboots. Bounded length: MQTT_ATTACHED_TOPICS_MAX.
  // Add via mqtt.attachTopic action; remove via mqtt.detachTopic.
  std::vector<String> attachedTopics;

  // runtime (not persisted)
  bool connected{false};
  String runtimeClientId;
  uint8_t disconnectReason{0};
  String topicsJoined;  // newline-separated rolling log (legacy field)

  // Sniffer mode: when ON, MqttSettingsService subscribes the broker
  // to `snifferPattern` (defaults to "#" = all topics). The ON flag
  // is runtime-only — does NOT persist across boot, by design: an
  // operator who left it on overnight on a busy broker shouldn't
  // slam reconnects with a flood-buffer of every retained message
  // every time the device boots. They re-arm it explicitly per
  // session. The pattern itself IS persisted — operator narrows
  // discovery (e.g. "home/+/temp/#") once, then toggles ON/OFF as
  // needed without retyping.
  bool   snifferEnabled{false};
  String snifferPattern{"#"};

  // Seen-topics ring — every inbound topic (whether matched by an
  // attached subscription or by the sniffer "#") gets upserted in
  // here. Capped at MQTT_TOPICS_LOG_MAX, oldest evicted on overflow.
  // Streamed to the UI one delta-row per WS tick (upsert by topic
  // key) so the frontend accumulates without ever serialising the
  // whole list in a single WS frame.
  struct SeenTopicEntry {
    String   topic;
    String   lastPayload;       // truncated to MQTT_SEEN_PAYLOAD_MAX chars
    uint32_t count{0};
    uint32_t lastSeenAt_s{0};
  };
  std::vector<SeenTopicEntry> seenTopics;
  // Topics that mutated since last WS tick — we emit ALL of them on
  // the next tick so a fast burst doesn't lose rows. Single-index
  // tracking would clobber: msg1 sets idx=A, msg2 sets idx=B before
  // serialise → row A never reaches the UI. Capped at SNIFFER_BURST
  // entries to bound JSON frame growth on retained-storm reconnects.
  // Stores topic NAMES (not indices) — robust to FIFO eviction in
  // seenTopics that would otherwise shift the indices.
  std::vector<String> pendingSeenTopics;
  static constexpr size_t SNIFFER_BURST_MAX = 32;

  // UI-transient: row-click on the seen table fills this with the
  // selected topic so the operator can hit "Attach" / "Detach" with
  // a single follow-up click. Not persisted.
  String selectedTopic;

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
    // client_id intentionally NOT persisted — recomputed every boot
    // from DeviceIdentity::canonical() so it can never drift out of
    // sync with the cert CN / mothership identity.
    root["keep_alive"] = s.keepAlive;
    root["clean_session"] = s.cleanSession;
    root["max_topic_length"] = s.maxTopicLength;
    JsonArray att = root.createNestedArray("attached_topics");
    for (const auto& t : s.attachedTopics) att.add(t);
    root["sniffer_pattern"] = s.snifferPattern;
  }

  /* ----- WS status reader (runtime + config echo) ----- */
  static void staRead(MqttSettings& s, JsonObject& root) {
    root["status"] = statusLabel(s);
    root["enabled"] = s.enabled;
    root["connected"] = s.connected;
    root["client_id"] = s.runtimeClientId;
    root["disconnect_reason"] = s.disconnectReason;
    root["topics"] = s.topicsJoined;
    root["sniffer_enabled"] = s.snifferEnabled;
    root["sniffer_pattern"] = s.snifferPattern;
    // INTENTIONALLY OMITTED: selected_topic. It's a UI-transient
    // field — frontend formState gets the topic via rowFill on row
    // click, then ActionField's withFields ships it through a query
    // string when Attach fires. Pushing it via WS would let the next
    // inbound message's broadcastWs blast an empty `_state.selected
    // Topic` back to the browser microseconds after the click,
    // wiping the formState before the operator can hit the button.

    // echo config so the settings tab stays in sync when another session edits
    root["host"] = s.host;
    root["port"] = s.port;
    root["username"] = s.username;
    root["pwd"] = s.password;
    root["client_id_cfg"] = s.clientId;
    root["keep_alive"] = s.keepAlive;
    root["clean_session"] = s.cleanSession;
    root["max_topic_length"] = s.maxTopicLength;

    // Streaming push: emit ALL topics that mutated since last tick
    // (capped at SNIFFER_BURST_MAX). tableMode("upsert") on the
    // frontend keys by `topic` and merges by row, accumulating up to
    // maxRows(MQTT_TOPICS_LOG_MAX). Same pattern the chart trends
    // use — frame size stays bounded regardless of broker volume.
    if (!s.pendingSeenTopics.empty()) {
      JsonArray seen = root.createNestedArray("seen_topics");
      for (const auto& t : s.pendingSeenTopics) {
        for (const auto& e : s.seenTopics) {
          if (e.topic != t) continue;
          JsonObject row = seen.createNestedObject();
          row["topic"]      = e.topic;
          row["lastValue"]  = e.lastPayload;
          row["count"]      = e.count;
          row["lastSeenAt"] = e.lastSeenAt_s;
          break;
        }
      }
      s.pendingSeenTopics.clear();
    }
  }

  static StateUpdateResult staUpd(JsonObject& in, MqttSettings& s) {
    return update(in, s);
  }

  /* ----- REST form (status + settings) ----- */
  static void buildForm(MqttSettings& s, JsonObject& root) {
    // STATUS
    JsonArray st = FormBuilder::createForm(root, "status", "Status");
    // Status avatar: green when broker connected, info (blue) when
    // intentionally disabled, error (red) for any enabled-but-not-
    // connected state. Driven by frontend `colorMap` so WS-pushed
    // value changes update the avatar live — `color()` would freeze
    // the avatar at REST-fetch time (operator who opens the tab
    // while disconnected then reconnects keeps seeing red until they
    // refresh). Disconnected reasons vary
    // ("Disconnected (TCP disconnected)" etc.) so we can't enumerate
    // them; `default` catches every disconnect variant as error.
    FormBuilder::addTextField(st, "status", AF::R, statusLabel(s).c_str(),
                              label("Status"), icon("DeviceHub"),
                              colorMap("Connected:success,Not enabled:info,default:error"));
    FormBuilder::addTextField(st, "client_id", AF::R, s.runtimeClientId.c_str(),
                              label("Client ID"), icon("Memory"));

    // ── Discovery controls — sit ABOVE the live table so the
    // operator's eye flow is: read the controls, then read the data
    // they produced. Sniffer toggle + pattern + selection + actions
    // are all in one cluster; the table below is the result of what
    // they configured here. ──

    // Sniffer toggle (runtime-only — see snifferEnabled field doc).
    FormBuilder::addSwitchField(st, "sniffer_enabled", AF::RW, s.snifferEnabled,
                                label("Sniffer"),
                                placeholder("Subscribe to all matching topics for discovery (resets on boot)"),
                                icon("Visibility"));
    // Sniffer pattern — defaults to "#" (everything). Narrow it to
    // e.g. "home/+/temp/#" to avoid the firehose. Persisted; the
    // ON/OFF toggle alone is per-session.
    FormBuilder::addTextField(st, "sniffer_pattern", AF::RW, s.snifferPattern.c_str(),
                              label("Sniffer pattern"),
                              placeholder("MQTT wildcard, e.g. # or home/+/temp/#"),
                              icon("FilterAlt"));

    // Selection mask — auto-filled by row-click on the live table
    // (rowFill="selected_topic=topic" below), or typed manually for
    // wildcard attaches like "home/+/temp/#". Attach/Detach buttons
    // ride this value through `withFields` into a query param so
    // the action sees what's currently in the form, not stale state.
    // Empty initial value on every form GET — selected_topic is a
    // pure UI-transient (see staRead comment). Server-side state
    // never holds a meaningful value here, so always emit "".
    FormBuilder::addTextField(st, "selected_topic", AF::RW, "",
                              label("Topic mask"),
                              placeholder("Click a row below or type a pattern"),
                              icon("CheckCircleOutline"));
    FormBuilder::addActionField(st, "attachSel", "Attach", AF::RW,
                                actionRef("mqtt.attachTopic"),
                                withFields("topic=selected_topic"),
                                icon("Add"), color("primary"), refetchForm());
    FormBuilder::addActionField(st, "detachSel", "Detach", AF::RW,
                                actionRef("mqtt.detachTopic"),
                                withFields("topic=selected_topic"),
                                icon("Remove"), color("warning"), refetchForm());

    // Live table — populated by sniffer + every attached subscription.
    // tableMode("upsert") merges by FIRST column (topic): the same
    // topic re-pushed updates that row's lastValue/count/age, a new
    // topic appends. Capped at MQTT_TOPICS_LOG_MAX. Row click writes
    // the topic into "selected_topic" above for one-click attach.
    FormBuilder::addTableField(
        st, "seen_topics", AF::R,
        col("topic",      "Topic",          "text"),
        col("lastValue",  "Value",          "text"),
        col("count",      "Count",          "number", format("0")),
        col("lastSeenAt", "Last (s)",       "number", format("0")),
        tableMode("upsert"), maxRows(MQTT_TOPICS_LOG_MAX),
        rowFill("selected_topic=topic"),
        icon("Topic"),
        label("Live topics (sniffer + attached)"));

    // SETTINGS — pure transport configuration. Discovery controls
    // (sniffer / mask / attach) live in Status because they belong
    // to the runtime-observation flow, not the connection setup.
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
    // Client ID is read-only — auto-derived from DeviceIdentity at every
    // connect so it stays in lock-step with the cert / mothership /
    // AutoUpdate identity. The Status tab's `client_id` row above shows
    // the same value while connected; this row mirrors it in the Settings
    // tab so the operator can confirm the broker-side identifier without
    // tab-switching.
    FormBuilder::addTextField(set, "client_id_cfg", AF::R, s.clientId.c_str(),
                              label("Client ID (auto)"), icon("Memory"));
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
    // client_id / client_id_cfg deliberately NOT read from the form —
    // the field is display-only; runtime value comes from begin() /
    // configureMqtt() via DeviceIdentity::canonical(). Older JSON
    // configs that still carry "client_id" are silently ignored.
    changed |= FormBuilder::updateValue(src, "keep_alive", s.keepAlive);
    changed |= FormBuilder::updateValue(src, "clean_session", s.cleanSession);
    changed |= FormBuilder::updateValue(src, "max_topic_length", s.maxTopicLength);
    changed |= FormBuilder::updateValue(src, "sniffer_enabled", s.snifferEnabled);
    changed |= FormBuilder::updateValue(src, "sniffer_pattern", s.snifferPattern);
    changed |= FormBuilder::updateValue(src, "selected_topic", s.selectedTopic);

    // attached_topics is mutated only via attach/detach actions —
    // not via plain form POST — but we still accept array reads
    // from disk via readConfig roundtrip. ConfigDelegate calls this
    // update() on load with the full persisted JSON, which contains
    // the array; honor it so attached topics survive reboot.
    if (root.containsKey("attached_topics") && root["attached_topics"].is<JsonArray>()) {
      JsonArray arr = root["attached_topics"].as<JsonArray>();
      std::vector<String> next;
      next.reserve(arr.size());
      for (JsonVariant v : arr) {
        String t = v.as<String>();
        if (t.length() > 0 && next.size() < MQTT_ATTACHED_TOPICS_MAX) next.push_back(t);
      }
      if (next != s.attachedTopics) {
        s.attachedTopics = std::move(next);
        changed = true;
      }
    }

    return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
  }
};

// Internal subscription record — provider-side state for each handle
// returned by subscribe(). Lives in MqttSettingsService::_subs vector;
// indexed by id (1-based; 0 = invalid id).
struct MqttSubscriptionRecord {
  uint32_t                       id{0};
  String                         name;
  MqttSubscriptionConfig         cfg;
  MqttSubscriptionStats          stats;
  bool                           enabled{true};
  // Sliding-window publish-rate limiter (mirrors Telegram's pattern).
  uint32_t                       windowStart_s{0};
  uint32_t                       windowSent{0};
  // Topic patterns this subscription has registered handlers for.
  // Patterns can include +/# wildcards; mqttTopicMatches() decides
  // dispatch on inbound. broker-side subscribe is done once per
  // unique pattern through the AsyncMqttClient regardless of how
  // many local handlers fire on it.
  struct Listener {
    String                          pattern;     // possibly with wildcards
    MqttSubscriptionMessageHandler  handler;
  };
  std::vector<Listener>          listeners;
};

/* ================= SERVICE ================= */
class MqttSettingsService : public StatefulService<MqttSettings>,
                             public IMqttDispatcher,
                             public IMqttProvider {
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

  // IMqttDispatcher (legacy callback model — kept working)
  void addMessageCallback(MqttMsgCb cb) override;
  void addConnectCallback(MqttConnectCb cb) override;
  void addDisconnectCallback(MqttDisconnectCb cb) override;

  // IMqttProvider — subscription model (preferred for new code)
  MqttSubscription subscribe(const char* serviceName,
                              const MqttSubscriptionConfig& cfg) override;
  ESPRack::MessagingSendId doPublish(uint32_t subId,
                                       const String& relativeTopic,
                                       const String& payload,
                                       const MqttPublishOptions& opt) override;
  void doSubscribeTopic(uint32_t subId,
                          const String& relativeTopic,
                          MqttSubscriptionMessageHandler handler) override;
  MqttSubscriptionStats statsForSubscription(uint32_t subId) const override;
  bool subscriptionEnabled(uint32_t subId) const override;
  void setSubscriptionEnabled(uint32_t subId, bool on) override;
  void releaseSubscription(uint32_t subId) override;
  const char* subscriptionName(uint32_t subId) const override;

  // IMessagingProvider (via IMqttProvider via IMessagingProvider)
  const char* providerId() const override { return "mqtt"; }
  ESPRack::MessagingConnectionState connectionState() const override;
  size_t subscriberCount() const override;

  const std::vector<String>& attachedTopics() const override {
    return _state.attachedTopics;
  }

  // Read-only iteration for UI rendering (mirrors TelegramService).
  template <typename Fn>
  void forEachSubscription(Fn cb) const {
    for (const auto& s : _subs) if (s.id != 0) cb(s);
  }

 protected:
  void onConfigUpdated();

 private:
  ConfigDelegate<MqttSettings> _cfg;
  WebFeatureEntry<MqttSettings>* _feature{nullptr};

  // Listeners (legacy IMqttDispatcher path)
  std::vector<MqttMsgCb> _msgListeners;
  std::vector<MqttConnectCb> _connectListeners;
  std::vector<MqttDisconnectCb> _disconnectListeners;

  // Subscription registry (new IMqttProvider path)
  std::vector<MqttSubscriptionRecord> _subs;
  uint32_t                            _nextSubId{1};

  // Helpers for subscription routing.
  MqttSubscriptionRecord*       findSub(uint32_t id);
  const MqttSubscriptionRecord* findSub(uint32_t id) const;
  bool checkAndConsumeQuota(MqttSubscriptionRecord& rec);
  // Dispatch one inbound message into all matching subscription
  // handlers. Called from onMqttMessage. Pattern matches use
  // mqttTopicMatches (wildcards-aware).
  void dispatchInboundToSubscribers(const String& topic, const String& payload);
  // Issue broker-side subscribe for a pattern if we don't already
  // have an in-flight subscription for it (broker dedups but we
  // avoid the round-trip when known).
  void brokerSubscribeIfNew(const String& pattern, uint8_t qos);
  std::set<String>  _brokerSubscribed;

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

 public:
  // Action-handler entry points. These are invoked from
  // registerManifest's WebActionSpec lambdas — exposed in the
  // public section so the lambdas can capture `this` and call them.
  // They mutate _state.attachedTopics and apply the broker-side
  // diff (subscribe/unsubscribe), then notify update handlers so
  // the persistence + WS broadcast fires. `topic` must come from
  // the operator-driven row-click (selectedTopic) — empty / oversize
  // / over-cap requests are silently dropped.
  void attachTopic(const String& topic);
  void detachTopic(const String& topic);

 private:
  // Sniffer + attached-topics machinery. _snifferActive tracks
  // whether the broker currently has a "#" subscription from us;
  // gets cleared on disconnect and re-applied in onMqttConnect if
  // the toggle is still on. Sniffer state is intentionally NOT
  // persisted (see MqttSettings::snifferEnabled doc).
  bool   _snifferActive{false};
  String _snifferActivePattern;

  // Transport snapshot used to decide whether a state-change
  // requires a full broker reconfigure. Toggling the sniffer or
  // editing selectedTopic must NOT bounce the connection — we
  // diff against this snapshot in addUpdateHandler and only call
  // onConfigUpdated() when a connection-level field actually moved.
  struct TransportSnapshot {
    bool     enabled{false};
    String   host;
    uint16_t port{0};
    String   username;
    String   password;
    String   clientId;
    uint16_t keepAlive{0};
    bool     cleanSession{false};
    uint16_t maxTopicLength{0};
    bool     valid{false};       // false until the first configureMqtt
  };
  TransportSnapshot _lastTransport;
  void captureTransportSnapshot();
  bool transportDiffersFromSnapshot() const;

  // Apply the sniffer toggle in-place when we're already connected:
  // single broker subscribe / unsubscribe on "#". Clears the seen
  // ring when sniffer turns off so the UI doesn't keep stale entries
  // long after the operator stopped looking. No-op when disconnected
  // (state will be applied in onMqttConnect when the session comes up).
  void applySnifferDiff();

  // Reconcile broker state with _state.attachedTopics: subscribe to
  // topics newly added since last apply, unsubscribe topics removed.
  // Tracks last-applied set in _attachedApplied — broker session
  // forgets on disconnect, so this is also called from onMqttConnect
  // to re-issue the full set.
  void applyAttachedTopicsDiff();
  std::vector<String> _attachedApplied;

  // Insert (or update) a seen-topic entry. Returns true if a WS push
  // is warranted (always true for now — the cap-and-evict logic
  // doesn't try to deduplicate WS pushes for the same topic within a
  // short window). Also sets _state.lastSeenChangedIdx so staRead
  // emits this entry on the next WS tick.
  bool upsertSeenTopic(const String& topic, const String& payload);
  void markPendingSeen(const String& topic);
};

#endif  // MqttSettingsService_h
