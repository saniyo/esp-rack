#include <MqttSettingsService.h>
#include <WebManager.h>
#include <DeviceIdentity.h>

// ─── mqttTopicMatches ───────────────────────────────────────────────
// MQTT v3.1.1 §4.7 wildcard matcher. Walks pattern + topic level by
// level: '+' eats one level, '#' eats remainder (must be the final
// level of the pattern). Empty levels are treated as their own level
// per the spec ("a//b" has three levels, the middle one empty).
//
// We re-match locally because AsyncMqttClient delivers every inbound
// message under the same callback regardless of which broker-side
// subscribe accepted it; the dispatcher needs to figure out which
// per-subscription handler(s) were the intended recipients.
bool mqttTopicMatches(const String& pattern, const String& topic) {
  size_t pi = 0, ti = 0;
  const size_t pn = pattern.length(), tn = topic.length();
  while (pi < pn) {
    // Multi-level '#' must be the last char (or '/<#>' suffix).
    if (pattern[pi] == '#') {
      // Per spec '#' is only legal as a complete level → either pi==0
      // or the previous char is '/'. Anything else is invalid pattern;
      // treat as no match.
      if (pi != 0 && pattern[pi - 1] != '/') return false;
      return pi == pn - 1;
    }

    // Take next levels.
    size_t pe = pattern.indexOf('/', pi); if (pe == (size_t)-1) pe = pn;
    size_t te = topic.indexOf('/', ti);   if (te == (size_t)-1) te = tn;

    bool isPlus = (pe - pi == 1) && (pattern[pi] == '+');

    if (!isPlus) {
      // Exact level match required.
      if (pe - pi != te - ti) return false;
      for (size_t k = 0; k < pe - pi; ++k) {
        if (pattern[pi + k] != topic[ti + k]) return false;
      }
    }

    pi = pe;
    ti = te;
    if (pi < pn && pattern[pi] == '/') pi++;
    if (ti < tn && topic[ti]   == '/') ti++;

    // If pattern ran out but topic hasn't (or vice versa), only OK if
    // both did simultaneously — caught by while-loop exit.
    if (pi >= pn && ti < tn) {
      // Pattern exhausted but topic has more levels → only matches if
      // we just consumed a trailing '#' (handled above) or the topic
      // had a trailing empty level. Otherwise not a match.
      return ti == tn;
    }
  }
  // Pattern exhausted; OK only if topic is also exhausted (or empty
  // trailing level).
  return ti >= tn;
}

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
    log_i("[mqtt.upd] origin='%s' enabled=%d connected=%d snifferOn=%d "
                  "attached.size=%u",
                  origin.c_str(), (int)_state.enabled, (int)_mqttClient.connected(),
                  (int)_state.snifferEnabled,
                  (unsigned)_state.attachedTopics.size());

    // Apply runtime-only diffs (sniffer toggle + attached-topics
    // membership) on the live broker session — these don't need
    // a transport tear-down. Doing them BEFORE the conditional
    // onConfigUpdated() means a sniffer toggle alone keeps the
    // broker session alive (no 5s reconnect blip).
    applySnifferDiff();
    applyAttachedTopicsDiff();

    // Trigger transport reconfigure ONLY when a connection-level
    // field actually changed since last configureMqtt. Without
    // this gate, every form save (including a sniffer flip or a
    // selected_topic change) would bounce the connection.
    if (transportDiffersFromSnapshot()) {
      log_d("[mqtt.upd] transport changed -> reconfigure");
      onConfigUpdated();
    }
    _cfg.saveIfChanged(origin);
  }, false);
}

MqttSettingsService::~MqttSettingsService() {
}

/* ---------- registerManifest ---------- */
void MqttSettingsService::registerManifest(WebManager* web) {
  if (!web) return;

  // Attach / Detach actions — fed by selected_topic in state. The
  // operator clicks a row in the seen / attached tables, the row's
  // topic auto-fills selected_topic via rowFill, then clicking the
  // button POSTs an empty body (the action just consumes current
  // state). refetchForm decorator on the action push rerenders the
  // tab so the attached-topics table updates without manual reload.
  WebActionSpec attachAct;
  attachAct.id              = "mqtt.attachTopic";
  attachAct.title           = "Attach";
  attachAct.icon            = "Add";
  attachAct.color           = "primary";
  attachAct.auth            = WebAuthLevel::Admin;
  attachAct.successMessage  = "Topic attached";
  attachAct.handler = [this](AsyncWebServerRequest* r) {
    // ActionField with `withFields("topic=selected_topic")` posts to
    // /attach?topic=<value>; backend reads from query param so the
    // operator's row-click selection round-trips even when the form
    // hasn't been Saved yet (Save would push selectedTopic into
    // _state, but action's POST has empty body otherwise).
    String t;
    if (r->hasArg("topic")) t = r->arg("topic");
    else                     t = _state.selectedTopic;
    log_d("[mqtt.attach] req topic-arg='%s' (hasArg=%d) state.selectedTopic='%s' params=%d",
                  t.c_str(), (int)r->hasArg("topic"), _state.selectedTopic.c_str(), r->params());
    // Dump every param the request carried so we can see exactly what
    // ActionField shipped (helps when query-string interpolation
    // misfires due to formState gap).
    for (size_t i = 0; i < r->params(); ++i) {
      const auto* p = r->getParam(i);
      log_d("[mqtt.attach]   param[%u] %s='%s'",
                    (unsigned)i, p->name().c_str(), p->value().c_str());
    }
    t.trim();
    attachTopic(t);
    r->send(200, "application/json", "{\"ok\":true}");
  };
  web->registerAction(attachAct);

  WebActionSpec detachAct;
  detachAct.id              = "mqtt.detachTopic";
  detachAct.title           = "Detach";
  detachAct.icon            = "Remove";
  detachAct.color           = "warning";
  detachAct.auth            = WebAuthLevel::Admin;
  detachAct.successMessage  = "Topic detached";
  detachAct.handler = [this](AsyncWebServerRequest* r) {
    String t;
    if (r->hasArg("topic")) t = r->arg("topic");
    else                     t = _state.selectedTopic;
    log_d("[mqtt.detach] req topic-arg='%s' (hasArg=%d)",
                  t.c_str(), (int)r->hasArg("topic"));
    t.trim();
    detachTopic(t);
    r->send(200, "application/json", "{\"ok\":true}");
  };
  web->registerAction(detachAct);

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

  WebTabSpec subsTab;
  subsTab.key      = "subs";
  subsTab.title    = "Subscriptions";
  subsTab.restPath = MQTT_FORM_PATH;
  subsTab.postable = false;
  subsTab.auth     = WebAuthLevel::Authenticated;
  subsTab.order    = 15;
  spec.tabs.push_back(subsTab);

  WebTabSpec settingsTab;
  settingsTab.key      = "settings";
  settingsTab.title    = "Settings";
  settingsTab.restPath = MQTT_FORM_PATH;
  settingsTab.postable = true;
  settingsTab.auth     = WebAuthLevel::Admin;
  settingsTab.order    = 20;
  spec.tabs.push_back(settingsTab);

  // Wrap the static buildForm so we can append a Subscriptions sub-
  // form without polluting the static reader (which is also called
  // by ConfigDelegate's secret-key probe). Mirrors the lambda
  // wrapper TelegramService uses.
  auto fullReader = [this](MqttSettings& s, JsonObject& root) {
    MqttSettings::buildForm(s, root);

    JsonArray subs = FormBuilder::createForm(root, "subs",
                                             "Internal services routing through this broker");
    JsonObject tbl = FormBuilder::addTableField(
        subs, "subscriptions", AF::R,
        col("name",       "Service",      "text"),
        col("prefix",     "Topic prefix", "text"),
        col("published",  "Published",    "number", format("0")),
        col("errors",     "Errors",       "number", format("0")),
        col("dropped",    "Dropped",      "number", format("0")),
        col("received",   "Received",     "number", format("0")),
        col("rate",       "Rate (msg/min)", "number", format("0")),
        col("lastPubAt",  "Last pub (s ago)", "number", format("0")),
        col("enabled",    "Enabled",      "text"),
        tableMode("replace"), maxRows(32),
        icon("Cable"),
        label("Active subscriptions"));
    JsonArray rows = tbl["subscriptions"].as<JsonArray>();
    uint32_t now_s = (uint32_t)(millis() / 1000);
    for (const auto& r : _subs) {
      if (r.id == 0) continue;
      JsonObject row = rows.createNestedObject();
      row["name"]      = r.name;
      row["prefix"]    = r.cfg.defaultTopicPrefix.length() ? r.cfg.defaultTopicPrefix : String("(none)");
      row["published"] = r.stats.published;
      row["errors"]    = r.stats.publishErrors;
      row["dropped"]   = r.stats.publishDropped;
      row["received"]  = r.stats.received;
      row["rate"]      = r.cfg.maxPublishesPerMinute;
      row["lastPubAt"] = r.stats.lastPublishAt_s == 0
                            ? 0
                            : (now_s > r.stats.lastPublishAt_s
                                ? now_s - r.stats.lastPublishAt_s
                                : 0);
      row["enabled"]   = r.enabled ? "yes" : "no";
    }

    // Persistent subscriptions — operator-attached topics that
    // survive reboot. Lives here on the Subscriptions tab next to
    // the internal-services table because both deal with "things
    // currently subscribed at the broker" — one auto (services),
    // one manual (operator). Click a row to seed `selected_topic`
    // for one-click Detach via the Status tab buttons. Last-value
    // column is filled by joining attachedTopics × seenTopics —
    // wildcards (+ / #) match many concrete topics so we can't show
    // "the" value for them, those rows display `(wildcard)`.
    JsonObject att = FormBuilder::addTableField(
        subs, "attached_topics_view", AF::R,
        col("topic", "Attached topic", "text"),
        col("value", "Last value",     "text"),
        tableMode("replace"), maxRows(MQTT_ATTACHED_TOPICS_MAX),
        rowFill("selected_topic=topic"),
        icon("PushPin"),
        label("Persistent subscriptions"));
    JsonArray attRows = att["attached_topics_view"].as<JsonArray>();
    for (const auto& t : s.attachedTopics) {
      JsonObject row = attRows.createNestedObject();
      row["topic"] = t;
      bool isWildcard = (t.indexOf('+') >= 0) || (t.indexOf('#') >= 0);
      if (isWildcard) {
        row["value"] = "(wildcard)";
      } else {
        String val;
        for (const auto& e : s.seenTopics) {
          if (e.topic == t) { val = e.lastPayload; break; }
        }
        row["value"] = val;  // empty if no message yet (broker has nothing retained)
      }
    }
    log_d("[mqtt.formBuild] attached_topics_view emitted %u rows (state=%u)",
                  attRows.size(), (unsigned)s.attachedTopics.size());
  };

  _feature = web->registerFeature<MqttSettings>(
      std::move(spec), this,
      fullReader,              MqttSettings::update,     // REST
      MqttSettings::staRead,   MqttSettings::staUpd,     // WS
      8192, 4096);
}

/* ---------- begin ---------- */
void MqttSettingsService::begin() {
  (void)_cfg.ensureLoaded();

  // ALWAYS overwrite whatever was loaded from /config/mqttSettings.json.
  // clientId is single-source-of-truth identity — operator cannot
  // override it, and any stale value an older firmware version saved
  // gets replaced here. See MqttSettingsService.h::clientId for the
  // rationale.
  _state.clientId = DeviceIdentity::canonical();

  if (_state.enabled) {
    log_d("MQTT is enabled, configuring...");
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
  log_d("Connected to MQTT, ");
  log_d("%s", sessionPresent ? "with persistent session" : "without persistent session");

  refreshRuntime();

  // Re-issue every broker-side subscribe pattern for non-persistent
  // sessions OR after a reconnect — without it the subscriber-side
  // handlers would silently stop firing after the first drop.
  // Cheaper than tracking "did the broker keep our subs": if the
  // session is fresh, we need to redo them; if persistent, the
  // broker dedups (per MQTT 3.1.1 §3.8.4 the spec allows clients to
  // refresh subscriptions).
  if (!sessionPresent) {
    for (const auto& rec : _subs) {
      if (rec.id == 0) continue;
      for (const auto& l : rec.listeners) {
        if (l.pattern.isEmpty()) continue;
        _mqttClient.subscribe(l.pattern.c_str(), rec.cfg.defaultQos);
      }
    }
  }

  // Re-apply attached topics on every (re)connect — broker forgets
  // them across disconnects regardless of cleanSession. We track our
  // own intent in _state.attachedTopics; resync from scratch.
  _attachedApplied.clear();
  applyAttachedTopicsDiff();

  // Re-apply sniffer if it was on. Reset cached flag first since
  // the broker session is fresh; applySnifferDiff handles pattern
  // resolution + empty-string fallback to "#".
  _snifferActive = false;
  _snifferActivePattern = "";
  applySnifferDiff();

  if (_feature) _feature->broadcastWs("mqtt");

  for (auto& cb : _connectListeners) cb(sessionPresent);
}

void MqttSettingsService::onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  log_w("[mqtt] disconnected reason=%u", (uint8_t)reason);

  _state.disconnectReason = (uint8_t)reason;
  _disconnectedAt = millis();

  // Broker session is gone — flush our cached "what we asked the
  // broker for" markers. onMqttConnect will re-issue from the
  // intent fields (_state.snifferEnabled / _state.attachedTopics).
  _snifferActive = false;
  _snifferActivePattern = "";
  _attachedApplied.clear();
  _brokerSubscribed.clear();

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
  String t = topic ? String(topic) : String();
  if (!t.isEmpty()) {
    bool firstTime = _discoveredTopics.insert(t).second;
    if (firstTime) {
      appendTopic(t);
    }

    // Upsert into the seen-topics ring regardless of first/repeat —
    // the table mode "upsert" keys on `topic` so a re-push of the
    // same topic just updates that row's count + lastValue. WS push
    // only fires when there's a UI subscriber (skip serialise on
    // unwatched tabs, same gating as legacy topics-list refresh).
    String body;
    if (payload && len > 0 && index == 0 && len == total) {
      size_t take = len > MQTT_SEEN_PAYLOAD_MAX ? MQTT_SEEN_PAYLOAD_MAX : len;
      body.reserve(take);
      for (size_t i = 0; i < take; ++i) body += payload[i];
      if (len > MQTT_SEEN_PAYLOAD_MAX) body += "...";
    }
    upsertSeenTopic(t, body);

    if (_feature && _feature->hasSubscribers()) _feature->broadcastWs("mqtt");
  }

  // Hand off to subscription handlers (new IMqttProvider model).
  // Only the single-shot delivery is supported at this layer — long
  // payloads with index/total fragmentation (>~10KB) still need the
  // legacy IMqttDispatcher path. AsyncMqttClient assembles short
  // messages itself (default 4KB), which covers 99% of telemetry.
  if (!t.isEmpty() && index == 0 && len == total) {
    String body;
    if (payload && len > 0) {
      body.reserve(len);
      for (size_t i = 0; i < len; ++i) body += payload[i];
    }
    dispatchInboundToSubscribers(t, body);
  }

  for (auto& cb : _msgListeners) cb(topic, payload, properties, len, index, total);
}

/* ---------- IMqttDispatcher ---------- */
void MqttSettingsService::addMessageCallback(MqttMsgCb cb)       { _msgListeners.push_back(cb); }
void MqttSettingsService::addConnectCallback(MqttConnectCb cb)   { _connectListeners.push_back(cb); }
void MqttSettingsService::addDisconnectCallback(MqttDisconnectCb cb) { _disconnectListeners.push_back(cb); }

/* ---------- IMqttProvider ---------- */
MqttSubscription MqttSettingsService::subscribe(
    const char* serviceName,
    const MqttSubscriptionConfig& cfg) {
  if (!serviceName || !*serviceName) return {};

  // Idempotent by name (mirrors Telegram). Preserves accumulated
  // counters across re-subscribes; only cfg fields are refreshed so
  // a service can adjust its prefix/QoS/rate limit between firmware
  // versions without losing telemetry.
  for (auto& rec : _subs) {
    if (rec.id != 0 && rec.name == serviceName) {
      rec.cfg = cfg;
      return MqttSubscription(rec.id, this);
    }
  }

  MqttSubscriptionRecord rec;
  rec.id      = _nextSubId++;
  rec.name    = serviceName;
  rec.cfg     = cfg;
  rec.enabled = true;
  _subs.push_back(std::move(rec));
  return MqttSubscription(_subs.back().id, this);
}

ESPRack::MessagingSendId MqttSettingsService::doPublish(
    uint32_t subId,
    const String& relativeTopic,
    const String& payload,
    const MqttPublishOptions& opt) {
  auto* rec = findSub(subId);
  if (!rec) return ESPRack::InvalidMessagingSendId;

  if (!rec->enabled) {
    rec->stats.publishDropped++;
    return ESPRack::InvalidMessagingSendId;
  }
  if (!_state.enabled || !_mqttClient.connected()) {
    rec->stats.publishDropped++;
    return ESPRack::InvalidMessagingSendId;
  }
  if (!checkAndConsumeQuota(*rec)) {
    rec->stats.publishDropped++;
    return ESPRack::InvalidMessagingSendId;
  }

  // Topic resolution: cfg prefix + relativeTopic. No automatic '/'
  // join — service is responsible for getting the boundary right
  // (see MqttSubscriptionConfig::defaultTopicPrefix doc).
  String full = rec->cfg.defaultTopicPrefix + relativeTopic;
  if (full.isEmpty()) {
    rec->stats.publishDropped++;
    return ESPRack::InvalidMessagingSendId;
  }

  uint8_t qos    = (opt.qos    >= 0) ? (uint8_t)opt.qos    : rec->cfg.defaultQos;
  bool    retain = (opt.retain >= 0) ? (opt.retain != 0)   : rec->cfg.defaultRetain;

  uint16_t pid = _mqttClient.publish(full.c_str(), qos, retain, payload.c_str());
  // AsyncMqttClient returns 0 on QoS-0 success too, so we can't use
  // the return value alone. We tag QoS-0 as "accepted optimistically"
  // because there's no ack path to discriminate from.
  if (pid == 0 && qos > 0) {
    rec->stats.publishErrors++;
    return ESPRack::InvalidMessagingSendId;
  }

  rec->stats.published++;
  rec->stats.lastPublishAt_s = (uint32_t)(millis() / 1000);

  // Tie SendId to per-subscription published count for traceability,
  // mirroring TelegramService::doSend.
  return rec->stats.published;
}

void MqttSettingsService::doSubscribeTopic(
    uint32_t subId,
    const String& relativeTopic,
    MqttSubscriptionMessageHandler handler) {
  if (!handler) return;
  auto* rec = findSub(subId);
  if (!rec) return;

  String full = rec->cfg.defaultTopicPrefix + relativeTopic;
  if (full.isEmpty()) return;

  // Register the local handler. Multiple onMessage calls on the same
  // subscription stack up — each gets dispatched if its pattern
  // matches the inbound topic (mqttTopicMatches honors '+' / '#').
  MqttSubscriptionRecord::Listener listener;
  listener.pattern = full;
  listener.handler = std::move(handler);
  rec->listeners.push_back(std::move(listener));

  // Tell the broker to actually deliver this pattern. We dedup
  // locally to avoid duplicate broker round-trips when two
  // subscriptions register the same pattern.
  brokerSubscribeIfNew(full, rec->cfg.defaultQos);
}

MqttSubscriptionStats MqttSettingsService::statsForSubscription(uint32_t subId) const {
  auto* rec = findSub(subId);
  return rec ? rec->stats : MqttSubscriptionStats{};
}

bool MqttSettingsService::subscriptionEnabled(uint32_t subId) const {
  auto* rec = findSub(subId);
  return rec ? rec->enabled : false;
}

void MqttSettingsService::setSubscriptionEnabled(uint32_t subId, bool on) {
  if (auto* rec = findSub(subId)) rec->enabled = on;
}

void MqttSettingsService::releaseSubscription(uint32_t subId) {
  for (auto& rec : _subs) {
    if (rec.id == subId) { rec.id = 0; break; }
  }
}

const char* MqttSettingsService::subscriptionName(uint32_t subId) const {
  auto* rec = findSub(subId);
  return rec ? rec->name.c_str() : "";
}

/* ---------- IMessagingProvider ---------- */
ESPRack::MessagingConnectionState MqttSettingsService::connectionState() const {
  if (!_state.enabled)                  return ESPRack::MessagingConnectionState::Disabled;
  if (_mqttClient.connected())          return ESPRack::MessagingConnectionState::Online;
  if (_disconnectedAt && _state.disconnectReason != 0)
                                         return ESPRack::MessagingConnectionState::Reconnecting;
  if (_disconnectedAt)                  return ESPRack::MessagingConnectionState::Connecting;
  return ESPRack::MessagingConnectionState::Idle;
}

size_t MqttSettingsService::subscriberCount() const {
  size_t n = 0;
  for (const auto& rec : _subs) if (rec.id != 0) ++n;
  return n;
}

/* ---------- subscription helpers ---------- */
MqttSubscriptionRecord* MqttSettingsService::findSub(uint32_t id) {
  if (id == 0) return nullptr;
  for (auto& rec : _subs) if (rec.id == id) return &rec;
  return nullptr;
}

const MqttSubscriptionRecord* MqttSettingsService::findSub(uint32_t id) const {
  if (id == 0) return nullptr;
  for (const auto& rec : _subs) if (rec.id == id) return &rec;
  return nullptr;
}

bool MqttSettingsService::checkAndConsumeQuota(MqttSubscriptionRecord& rec) {
  if (rec.cfg.maxPublishesPerMinute == 0) return true;

  uint32_t now_s = (uint32_t)(millis() / 1000);
  if (rec.windowStart_s == 0 || now_s - rec.windowStart_s >= 60) {
    rec.windowStart_s = now_s;
    rec.windowSent    = 0;
  }
  if (rec.windowSent >= rec.cfg.maxPublishesPerMinute) return false;
  rec.windowSent++;
  return true;
}

void MqttSettingsService::dispatchInboundToSubscribers(const String& topic, const String& payload) {
  for (auto& rec : _subs) {
    if (rec.id == 0 || !rec.enabled) continue;
    bool matched = false;
    for (auto& l : rec.listeners) {
      if (mqttTopicMatches(l.pattern, topic)) {
        if (l.handler) l.handler(topic, payload);
        matched = true;
      }
    }
    if (matched) {
      rec.stats.received++;
      rec.stats.lastReceiveAt_s = (uint32_t)(millis() / 1000);
    }
  }
}

void MqttSettingsService::brokerSubscribeIfNew(const String& pattern, uint8_t qos) {
  if (pattern.isEmpty()) return;
  if (_brokerSubscribed.count(pattern)) return;
  _brokerSubscribed.insert(pattern);
  // Only push to the broker when we're actually online — otherwise
  // we'll re-issue all of them in onMqttConnect when the session
  // (re)comes up. AsyncMqttClient itself drops subscribe calls when
  // not connected.
  if (_mqttClient.connected()) {
    _mqttClient.subscribe(pattern.c_str(), qos);
  }
}

void MqttSettingsService::onConfigUpdated() {
  _reconfigureMqtt = true;
  _disconnectedAt = 0;
}

#ifdef ESP32
void MqttSettingsService::onStationModeGotIP(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (_state.enabled) {
    log_i("WiFi connected, starting MQTT client.");
    onConfigUpdated();
  }
}
void MqttSettingsService::onStationModeDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (_state.enabled) {
    log_d("WiFi dropped, stopping MQTT client.");
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
    log_d("Connecting to MQTT...");
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

  // Capture what we just applied so future addUpdateHandler runs
  // can short-circuit the reconfigure path when only runtime fields
  // (sniffer, selected_topic) change.
  captureTransportSnapshot();

  refreshRuntime();
  if (_feature) _feature->broadcastWs("mqtt");
}

/* ---------- runtime mirrors ---------- */
void MqttSettingsService::refreshRuntime() {
  _state.connected = _mqttClient.connected();
  const char* cid = _mqttClient.getClientId();
  _state.runtimeClientId = cid ? cid : "";
}

/* ---------- transport-snapshot helpers ---------- */
void MqttSettingsService::captureTransportSnapshot() {
  _lastTransport.enabled        = _state.enabled;
  _lastTransport.host           = _state.host;
  _lastTransport.port           = _state.port;
  _lastTransport.username       = _state.username;
  _lastTransport.password       = _state.password;
  _lastTransport.clientId       = _state.clientId;
  _lastTransport.keepAlive      = _state.keepAlive;
  _lastTransport.cleanSession   = _state.cleanSession;
  _lastTransport.maxTopicLength = _state.maxTopicLength;
  _lastTransport.valid          = true;
}

bool MqttSettingsService::transportDiffersFromSnapshot() const {
  if (!_lastTransport.valid) return true;  // first run — treat as different
  return _state.enabled        != _lastTransport.enabled
      || _state.host           != _lastTransport.host
      || _state.port           != _lastTransport.port
      || _state.username       != _lastTransport.username
      || _state.password       != _lastTransport.password
      || _state.clientId       != _lastTransport.clientId
      || _state.keepAlive      != _lastTransport.keepAlive
      || _state.cleanSession   != _lastTransport.cleanSession
      || _state.maxTopicLength != _lastTransport.maxTopicLength;
}

/* ---------- sniffer / attached helpers ---------- */
void MqttSettingsService::applySnifferDiff() {
  if (!_mqttClient.connected()) {
    _snifferActive = false;
    return;
  }

  // Compute the desired pattern. Empty user input falls back to "#"
  // so a wiped field doesn't accidentally drop the discovery feed
  // mid-session — operator must turn the toggle OFF to stop sniffing.
  String wantPattern = _state.snifferPattern;
  wantPattern.trim();
  if (wantPattern.length() == 0) wantPattern = "#";

  bool wantOn = _state.snifferEnabled;

  // Three transitions matter: turn-on, turn-off, and pattern-change-
  // while-on. Pattern-change is a single un-then-resub on the broker.
  bool needRestart = _snifferActive && wantOn && _snifferActivePattern != wantPattern;

  if (_snifferActive && (!wantOn || needRestart)) {
    if (_snifferActivePattern.length()) {
      _mqttClient.unsubscribe(_snifferActivePattern.c_str());
    }
    _snifferActive = false;
    _snifferActivePattern = "";

    // Drop accumulated discovery state when stopping or repointing.
    // Attached subscriptions keep their own re-arrival path, so the
    // ones operator pinned will reappear naturally as their messages
    // come in. Clears UI clutter from the previous pattern.
    _state.seenTopics.clear();
    _state.pendingSeenTopics.clear();
    _state.topicsJoined = "";
    _discoveredTopics.clear();
  }

  if (wantOn && !_snifferActive) {
    _mqttClient.subscribe(wantPattern.c_str(), 0);
    _snifferActive = true;
    _snifferActivePattern = wantPattern;
  }
}

void MqttSettingsService::applyAttachedTopicsDiff() {
  if (!_mqttClient.connected()) {
    log_w("[mqtt.attDiff] not connected, skip (want=%u applied=%u)",
                  (unsigned)_state.attachedTopics.size(),
                  (unsigned)_attachedApplied.size());
    return;
  }

  // Subscribe to new entries.
  for (const auto& t : _state.attachedTopics) {
    bool already = false;
    for (const auto& a : _attachedApplied) { if (a == t) { already = true; break; } }
    if (!already && t.length() > 0) {
      uint16_t pid = _mqttClient.subscribe(t.c_str(), 0);
      log_d("[mqtt.attDiff] subscribe '%s' pid=%u", t.c_str(), pid);
      _attachedApplied.push_back(t);
    }
  }

  // Unsubscribe from removed entries.
  std::vector<String> next;
  next.reserve(_attachedApplied.size());
  for (const auto& a : _attachedApplied) {
    bool stillWanted = false;
    for (const auto& t : _state.attachedTopics) { if (t == a) { stillWanted = true; break; } }
    if (stillWanted) {
      next.push_back(a);
    } else if (a.length() > 0) {
      uint16_t pid = _mqttClient.unsubscribe(a.c_str());
      log_d("[mqtt.attDiff] unsubscribe '%s' pid=%u", a.c_str(), pid);
    }
  }
  _attachedApplied = std::move(next);
  log_i("[mqtt.attDiff] done, want=%u applied=%u",
                (unsigned)_state.attachedTopics.size(),
                (unsigned)_attachedApplied.size());
}

bool MqttSettingsService::upsertSeenTopic(const String& topic, const String& payload) {
  // Find existing entry by topic key — update in place, mark for emit.
  for (auto& e : _state.seenTopics) {
    if (e.topic == topic) {
      e.lastPayload  = payload;
      e.count++;
      e.lastSeenAt_s = (uint32_t)(millis() / 1000);
      markPendingSeen(topic);
      return true;
    }
  }

  // New entry — evict oldest if at cap. FIFO insertion-order
  // eviction (not LRU) — operator's discovery list, not a heat map.
  // If a noisy topic kicks out one the operator cares about, they
  // can attach it (which makes it survive across the seen-ring
  // entirely; attached topics get their own re-arrival path).
  if (_state.seenTopics.size() >= MQTT_TOPICS_LOG_MAX) {
    const String evicted = _state.seenTopics.front().topic;
    _state.seenTopics.erase(_state.seenTopics.begin());
    // Drop the evicted topic from pending queue too — we won't be
    // able to find it in seenTopics on emit.
    for (auto it = _state.pendingSeenTopics.begin(); it != _state.pendingSeenTopics.end(); ) {
      if (*it == evicted) it = _state.pendingSeenTopics.erase(it); else ++it;
    }
  }
  MqttSettings::SeenTopicEntry e;
  e.topic        = topic;
  e.lastPayload  = payload;
  e.count        = 1;
  e.lastSeenAt_s = (uint32_t)(millis() / 1000);
  _state.seenTopics.push_back(std::move(e));
  markPendingSeen(topic);
  return true;
}

void MqttSettingsService::markPendingSeen(const String& topic) {
  // Dedup: if topic already pending, leave it (its data in seenTopics
  // is already up to date; one row will be emitted with latest state).
  for (const auto& p : _state.pendingSeenTopics) if (p == topic) return;
  if (_state.pendingSeenTopics.size() >= MqttSettings::SNIFFER_BURST_MAX) {
    // Drop oldest pending — caller's row will arrive on the next
    // upsert anyway; keeping a hard cap prevents unbounded growth
    // during retained-storm reconnects.
    _state.pendingSeenTopics.erase(_state.pendingSeenTopics.begin());
  }
  _state.pendingSeenTopics.push_back(topic);
}

/* ---------- attach / detach actions ---------- */
void MqttSettingsService::attachTopic(const String& topic) {
  if (topic.isEmpty()) {
    log_d("[mqtt.attach] EARLY-RETURN: topic empty");
    return;
  }
  if (topic.length() > 256) {
    log_d("[mqtt.attach] EARLY-RETURN: topic too long (%u chars)", topic.length());
    return;
  }
  if (_state.attachedTopics.size() >= MQTT_ATTACHED_TOPICS_MAX) {
    log_d("[mqtt.attach] EARLY-RETURN: at cap (%u/%u)",
                  (unsigned)_state.attachedTopics.size(), (unsigned)MQTT_ATTACHED_TOPICS_MAX);
    return;
  }
  for (const auto& t : _state.attachedTopics) {
    if (t == topic) {
      log_i("[mqtt.attach] EARLY-RETURN: already attached '%s'", topic.c_str());
      return;
    }
  }

  log_d("[mqtt.attach] mutating state with '%s' (current size=%u)",
                topic.c_str(), (unsigned)_state.attachedTopics.size());

  // Mutate via the StatefulService update API so persistence + WS
  // broadcast + addUpdateHandler chain run correctly. We capture a
  // copy because the caller's `topic` may be a reference into
  // _state.selectedTopic which the update lambda might rewrite.
  String t = topic;
  update([t](MqttSettings& s) {
    s.attachedTopics.push_back(t);
    return StateUpdateResult::CHANGED;
  }, "attach");

  log_d("[mqtt.attach] post-update size=%u, last='%s'",
                (unsigned)_state.attachedTopics.size(),
                _state.attachedTopics.empty() ? "" : _state.attachedTopics.back().c_str());
}

void MqttSettingsService::detachTopic(const String& topic) {
  if (topic.isEmpty()) return;
  bool found = false;
  for (const auto& t : _state.attachedTopics) {
    if (t == topic) { found = true; break; }
  }
  if (!found) return;

  String t = topic;
  update([t](MqttSettings& s) {
    for (auto it = s.attachedTopics.begin(); it != s.attachedTopics.end(); ++it) {
      if (*it == t) { s.attachedTopics.erase(it); break; }
    }
    return StateUpdateResult::CHANGED;
  }, "detach");
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
