#include "TelegramService.h"
#include <ArduinoJson.h>
#include <WebManager.h>
#include <WebFeatureSpec.h>
#include <FormBuilder.h>

// ─── ctor / dtor ────────────────────────────────────────────────────
TelegramService::TelegramService(ConfigManager* cfgMgr)
  : StatefulService<TelegramSettings>(),
    _cfg(cfgMgr,
         "telegram",
         TEL_FILE,
         4096,
         this,
         TelegramSettings::readConfig,
         TelegramSettings::upd,
         false /*autoSave*/,
         nullptr /*validator*/,
         TelegramSettings::buildForm /*formReader*/),
    _cli(),
    _bot("", _cli),
    _botTokenInBot("") {
  _q = xQueueCreate(TEL_QUEUE_DEPTH, sizeof(TelegramQueuedMessage));
  // Reserve modest registry capacity so the first ~8 subscribers
  // don't trigger vector reallocs (which would invalidate any
  // in-flight pointers from findSub() — kept short & sweet).
  _subs.reserve(8);
}

TelegramService::~TelegramService() {
  if (_task) vTaskDelete(_task);
  if (_q)    vQueueDelete(_q);
}

// ─── manifest + action endpoint ─────────────────────────────────────
void TelegramService::registerManifest(WebManager* web) {
  if (!web) return;

  WebActionSpec sendManual;
  sendManual.id              = "telegram.sendManual";
  sendManual.title           = "Send";
  sendManual.icon            = "Send";
  sendManual.color           = "primary";
  sendManual.auth            = WebAuthLevel::Admin;
  sendManual.successMessage  = "Message queued";
  sendManual.handler = [this](AsyncWebServerRequest* r) {
    sendManualNow();
    r->send(200, "application/json", "{\"ok\":true}");
  };
  web->registerAction(sendManual);

  WebFeatureSpec spec;
  spec.id         = "telegram";
  spec.title      = "Telegram";
  spec.component  = "DynamicSettings";
  spec.menu.label = "Telegram";
  spec.menu.icon  = "Telegram";
  spec.menu.order = 400;
  spec.menu.auth  = WebAuthLevel::Admin;
  spec.auth       = WebAuthLevel::Admin;
  spec.restRead   = TEL_FORM_PATH;
  spec.restUpdate = TEL_FORM_PATH;
  spec.wsPath     = TEL_WS_PATH;

  WebTabSpec statusTab;
  statusTab.key      = "status";
  statusTab.title    = "Status";
  statusTab.restPath = TEL_FORM_PATH;
  statusTab.postable = false;
  statusTab.live     = true;
  statusTab.auth     = WebAuthLevel::Admin;
  spec.tabs.push_back(statusTab);

  WebTabSpec subsTab;
  subsTab.key      = "subs";
  subsTab.title    = "Subscriptions";
  subsTab.restPath = TEL_FORM_PATH;
  subsTab.postable = false;
  subsTab.auth     = WebAuthLevel::Admin;
  subsTab.order    = 15;
  spec.tabs.push_back(subsTab);

  WebTabSpec settingsTab;
  settingsTab.key      = "settings";
  settingsTab.title    = "Settings";
  settingsTab.restPath = TEL_FORM_PATH;
  settingsTab.postable = true;
  settingsTab.auth     = WebAuthLevel::Admin;
  settingsTab.order    = 20;
  spec.tabs.push_back(settingsTab);

  // Wrap the static form reader in a lambda that appends the
  // Subscriptions sub-form. Static buildForm doesn't know about
  // subscriptions (and shouldn't — it's also used by ConfigDelegate
  // for the secret-key probe, which mustn't traverse runtime state).
  // The lambda renders Status + Settings via the static reader, then
  // adds a Subscriptions sub-form on top, populated from _subs.
  auto fullReader = [this](TelegramSettings& s, JsonObject& root) {
    TelegramSettings::buildForm(s, root);

    JsonArray subs = FormBuilder::createForm(root, "subs",
                                             "Internal services streaming through this bot");
    JsonObject tbl = FormBuilder::addTableField(
        subs, "subscriptions", AF::R,
        col("name",      "Service",      "text"),
        col("sent",      "Sent",         "number", format("0")),
        col("errors",    "Errors",       "number", format("0")),
        col("dropped",   "Dropped",      "number", format("0")),
        col("rate",      "Rate (msg/min)", "number", format("0")),
        col("lastAt",    "Last (s ago)", "number", format("0")),
        col("topic",     "Topic ID",     "text"),
        col("enabled",   "Enabled",      "text"),
        tableMode("replace"), maxRows(32),
        icon("Cable"),
        label("Active subscriptions"));
    JsonArray rows = tbl["subscriptions"].as<JsonArray>();
    uint32_t now_s = (uint32_t)(millis() / 1000);
    for (const auto& r : _subs) {
      if (r.id == 0) continue;
      JsonObject row = rows.createNestedObject();
      row["name"]     = r.name;
      row["sent"]     = r.stats.sent;
      row["errors"]   = r.stats.errors;
      row["dropped"]  = r.stats.dropped;
      row["rate"]     = r.cfg.maxMessagesPerMinute;
      row["lastAt"]   = r.stats.lastSentAt_s == 0
                            ? 0
                            : (now_s > r.stats.lastSentAt_s
                                ? now_s - r.stats.lastSentAt_s
                                : 0);
      row["topic"]    = r.cfg.defaultTopicId.length() ? r.cfg.defaultTopicId : String("(default)");
      row["enabled"]  = r.enabled ? "yes" : "no";
    }
  };

  // 24k REST buffer — earlier 12KB was on the edge once the chat log
  // saturated, leading to silent JSON truncation that made the
  // Status tab hang in "Loading…" forever after a few service
  // sends filled the log. 24KB leaves comfortable headroom for log
  // (capped at 30 rows) + subs table + settings form. WS stays at
  // 8k since it pushes only status+log (no subs), and is throttled
  // by the action cadence anyway.
  _feature = web->registerFeature<TelegramSettings>(
      std::move(spec), this,
      fullReader,                 TelegramSettings::upd,
      TelegramSettings::staRead,  TelegramSettings::staUpd,
      24576, 8192);
}

void TelegramService::begin() {
  (void)_cfg.ensureLoaded();
  _cli.setInsecure();
  ensureBotToken(_state.botToken);
  updateStatusLabel();

  // Persistence + UI sync handler — fires on every state change
  // (REST POST, WS staUpd, internal mutations from worker / send-
  // result accounting). Two responsibilities:
  //   1. _cfg.saveIfChanged(origin) — commits dirty config to disk.
  //      Without it, REST Save in the form only updates RAM and
  //      reboot wipes everything (which is exactly what the user
  //      hit on the first round of testing).
  //   2. Refresh statusLabel + broadcast WS — operator toggling
  //      "Enabled" should see status flip from "Disabled" → "Idle"
  //      immediately, not only after the first message tries to
  //      send. ensureBotToken catches token edits so the UTB
  //      fallback uses the new value next time.
  addUpdateHandler([this](const String& origin) {
    ensureBotToken(_state.botToken);
    updateStatusLabel();
    _cfg.saveIfChanged(origin);
    if (_feature) _feature->broadcastWs(origin);
  }, false);

#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
  xTaskCreate(taskTrampoline, "TelTask", 6144, this, 1, &_task);
#else
  xTaskCreatePinnedToCore(taskTrampoline, "TelTask", 6144, this, 1, &_task, 1);
#endif

  if (_feature) _feature->broadcastWs("boot");
}

// ─── ITelegramProvider — subscription registry ──────────────────────
TelegramSubscription TelegramService::subscribe(
    const char* serviceName,
    const TelegramSubscriptionConfig& cfg) {
  if (!serviceName || !*serviceName) return {};

  // Idempotent by name — re-subscribing with the same name keeps
  // accumulated counters but updates cfg fields. Lets a service
  // tweak its rate limit between firmware versions without losing
  // its history.
  for (auto& rec : _subs) {
    if (rec.id != 0 && rec.name == serviceName) {
      rec.cfg = cfg;
      return TelegramSubscription(rec.id, this);
    }
  }

  TelegramSubscriptionRecord rec;
  rec.id      = _nextSubId++;
  rec.name    = serviceName;
  rec.cfg     = cfg;
  rec.enabled = true;
  _subs.push_back(std::move(rec));
  return TelegramSubscription(_subs.back().id, this);
}

ESPRack::MessagingSendId TelegramService::doSend(
    uint32_t subId,
    const TelegramRecipient& explicitTo,
    const String& text,
    const TelegramSendOptions& opt) {
  if (text.isEmpty()) return ESPRack::InvalidMessagingSendId;

  auto* rec = findSub(subId);
  if (!rec) return ESPRack::InvalidMessagingSendId;

  if (!rec->enabled) {
    rec->stats.dropped++;
    return ESPRack::InvalidMessagingSendId;
  }
  if (!checkAndConsumeQuota(*rec)) {
    rec->stats.dropped++;
    logLine(false, "drop", String("[") + rec->name + "] quota exhausted");
    callUpdateHandlers("sta");
    return ESPRack::InvalidMessagingSendId;
  }

  TelegramQueuedMessage m;
  m.subId          = rec->id;
  m.parseMode      = opt.parseMode;
  m.silent         = opt.silent;
  // Routing precedence: explicit per-call > subscription default >
  // bot global default. Empty fields propagate down through the
  // worker which falls back to _state.{chatId,topicId,botToken}.
  m.chatOverride   = !explicitTo.chatId.isEmpty()  ? explicitTo.chatId  : rec->cfg.defaultChatId;
  m.topicOverride  = !explicitTo.topicId.isEmpty() ? explicitTo.topicId : rec->cfg.defaultTopicId;

  // Apply tag prefix. Empty tagPrefix → use "<name>: " auto-prefix
  // (Markdown-safe — brackets [...] would be interpreted as the
  // start of a `[text](url)` link in parseMode=Markdown and
  // Telegram returns 400 "can't parse entities", silently
  // swallowing the message. Bot users saw no traffic from any
  // service that auto-prefixed; manual sends — without a prefix —
  // worked. Colon-style prefix is safe across all parseModes
  // (None / Markdown / MarkdownV2 / HTML), so consumers don't
  // need to think about formatting just to identify themselves.
  // Caller wanting NO prefix sets cfg.tagPrefix = " " explicitly.
  String prefix = rec->cfg.tagPrefix;
  if (prefix.length() == 0) {
    prefix = rec->name + String(": ");
  }
  m.text = prefix + text;

  if (xQueueSend(_q, &m, 0) != pdTRUE) {
    rec->stats.dropped++;
    logLine(false, "drop", String("[") + rec->name + "] queue full");
    callUpdateHandlers("sta");
    return ESPRack::InvalidMessagingSendId;
  }

  _state.qSize = uxQueueMessagesWaiting(_q);
  // Optimistic stats — confirmed by worker on actual send. We use
  // sentCount as "in-flight queued + delivered" combined, with errors
  // breaking out failures separately. UI shows both columns so the
  // operator can spot a service that's queueing fine but failing on
  // the wire.
  rec->stats.sent++;
  rec->stats.lastSentAt_s = (uint32_t)(millis() / 1000);
  callUpdateHandlers("sta");

  // SendId is a global counter aliased to bot's lifetime sent total.
  // Tied to per-subscription stats.sent for traceability.
  return rec->stats.sent;
}

TelegramSubscriptionStats TelegramService::statsForSubscription(uint32_t subId) const {
  auto* rec = findSub(subId);
  return rec ? rec->stats : TelegramSubscriptionStats{};
}

bool TelegramService::subscriptionEnabled(uint32_t subId) const {
  auto* rec = findSub(subId);
  return rec ? rec->enabled : false;
}

void TelegramService::setSubscriptionEnabled(uint32_t subId, bool on) {
  if (auto* rec = findSub(subId)) rec->enabled = on;
}

void TelegramService::releaseSubscription(uint32_t subId) {
  for (auto& rec : _subs) {
    if (rec.id == subId) { rec.id = 0; break; }
  }
}

const char* TelegramService::subscriptionName(uint32_t subId) const {
  auto* rec = findSub(subId);
  return rec ? rec->name.c_str() : "";
}

// ─── IMessagingProvider ─────────────────────────────────────────────
ESPRack::MessagingConnectionState TelegramService::connectionState() const {
  if (!_state.enabled) return ESPRack::MessagingConnectionState::Disabled;
  if (_state.qSize > 0) return ESPRack::MessagingConnectionState::Connecting;
  if (_state.lastSendAt_s > 0) {
    return _state.lastSendOk
        ? ESPRack::MessagingConnectionState::Online
        : ESPRack::MessagingConnectionState::Reconnecting;
  }
  return ESPRack::MessagingConnectionState::Idle;
}

size_t TelegramService::subscriberCount() const {
  size_t n = 0;
  for (const auto& rec : _subs) if (rec.id != 0) ++n;
  return n;
}

// ─── Public legacy API (kept working through subscriptions) ─────────
void TelegramService::enqueueDefault(const char* text) {
  if (!text) return;
  enqueueMessage(String(), String(), String(text));
}

void TelegramService::enqueueMessage(const String& chatId,
                                     const String& topicId,
                                     const String& text,
                                     const TelegramSendOptions& opt,
                                     const String& tokenOverride) {
  if (text.isEmpty()) return;

  TelegramQueuedMessage m;
  m.subId          = 0;  // legacy path — no subscription accounting
  m.chatOverride   = chatId;
  m.topicOverride  = topicId;
  m.tokenOverride  = tokenOverride;
  m.text           = text;
  m.parseMode      = opt.parseMode;
  m.silent         = opt.silent;

  if (xQueueSend(_q, &m, 0) != pdTRUE) {
    logLine(false, "drop", String("[manual] queue full: ") + text);
    callUpdateHandlers("sta");
    return;
  }
  _state.qSize = uxQueueMessagesWaiting(_q);
  logLine(false, "queued", text);
  callUpdateHandlers("sta");
}

void TelegramService::sendManualNow() {
  if (!_state.enabled) {
    logLine(false, "skip", "Bot disabled");
    callUpdateHandlers("sta");
    return;
  }
  if (_state.manualText.isEmpty()) return;

  String pending = _state.manualText;
  _state.manualText = "";
  enqueueMessage(String(), String(), pending);
  callUpdateHandlers("sta");
}

// ─── helpers ────────────────────────────────────────────────────────
TelegramSubscriptionRecord* TelegramService::findSub(uint32_t id) {
  if (id == 0) return nullptr;
  for (auto& rec : _subs) {
    if (rec.id == id) return &rec;
  }
  return nullptr;
}

const TelegramSubscriptionRecord* TelegramService::findSub(uint32_t id) const {
  if (id == 0) return nullptr;
  for (const auto& rec : _subs) {
    if (rec.id == id) return &rec;
  }
  return nullptr;
}

bool TelegramService::checkAndConsumeQuota(TelegramSubscriptionRecord& rec) {
  if (rec.cfg.maxMessagesPerMinute == 0) return true;  // unlimited

  uint32_t now_s = (uint32_t)(millis() / 1000);
  if (rec.windowStart_s == 0 || now_s - rec.windowStart_s >= 60) {
    rec.windowStart_s = now_s;
    rec.windowSent    = 0;
  }
  if (rec.windowSent >= rec.cfg.maxMessagesPerMinute) return false;
  rec.windowSent++;
  return true;
}

void TelegramService::applyDefaults(const TelegramSubscriptionRecord& rec,
                                    TelegramRecipient& to,
                                    String& text) {
  if (to.chatId.isEmpty())  to.chatId  = rec.cfg.defaultChatId;
  if (to.topicId.isEmpty()) to.topicId = rec.cfg.defaultTopicId;
  // text already prefixed by doSend; nothing further here.
  (void)text;
}

// ─── transport ──────────────────────────────────────────────────────
//
// IMPORTANT: every blocking call here MUST have an explicit timeout.
// Earlier we relied on Arduino's defaults — and on flaky / blocked
// networks (corporate proxies, ISP filtering of telegram.org, mobile
// hotspots with deep TLS inspection) the worker would stall forever
// inside _cli.connect() or readStringUntil(). qSize stuck at 1, no
// log entry written, AsyncWS clients on the same Wi-Fi radio
// starving for transmit windows → LiveIndicator flickering. Adding
// timeouts converts an unbounded stall into a clean "fail" log line
// + queue drain in ≤8s worst case.
bool TelegramService::sendDirect(const String& token,
                                 const String& chatId,
                                 const String& topicId,
                                 const String& text,
                                 int parseMode,
                                 bool silent) {
  if (token.isEmpty() || chatId.isEmpty()) return false;

  const char* host = "api.telegram.org";
  const int   port = 443;

  // Guarantee a clean socket state — a previous abandoned connect
  // (timeout mid-handshake) can leave _cli in a half-open state where
  // the next connect()'s underlying lwIP socket fails to acquire and
  // hangs. stop() is idempotent on a closed socket.
  _cli.stop();

  // TLS-handshake timeout — argument is SECONDS for setHandshakeTimeout.
  // 8s covers slow handshakes on weak Wi-Fi without giving up too early.
  _cli.setHandshakeTimeout(8);
  // Read-operation timeout in milliseconds. readStringUntil
  // / read() / available() polls won't block past this.
  _cli.setTimeout(5000);

  // Connect with explicit 5s wall clock. Without this overload Arduino
  // uses its global socket timeout which on ESP-IDF 5 is effectively
  // unbounded for some lwIP configurations.
  if (!_cli.connect(host, port, 5000)) return false;

  StaticJsonDocument<1024> body;
  body["chat_id"] = chatId;
  body["text"]    = text;
  if (const char* pm = parseModeString(parseMode)) body["parse_mode"] = pm;
  if (!topicId.isEmpty()) body["message_thread_id"] = topicId.toInt();
  if (silent)             body["disable_notification"] = true;

  String payload;
  serializeJson(body, payload);

  String url = String("/bot") + token + "/sendMessage";
  String req =
      String("POST ") + url + " HTTP/1.1\r\n" +
      "Host: " + host + "\r\n" +
      "User-Agent: ESP32\r\n" +
      "Content-Type: application/json\r\n" +
      "Connection: close\r\n" +
      "Content-Length: " + String(payload.length()) + "\r\n\r\n" +
      payload;

  _cli.print(req);

  String statusLine = _cli.readStringUntil('\n');
  bool ok = statusLine.indexOf(" 200 ") >= 0;

  // Drain the rest with a wall clock — readStringUntil could leave
  // bytes in the buffer; without a bounded loop we'd spin until the
  // remote closes. setTimeout above already caps individual read()s,
  // and `available()` returns 0 when the FIN+RST has landed, so this
  // loop terminates.
  uint32_t drainStart = millis();
  while ((_cli.connected() || _cli.available()) && (millis() - drainStart < 2000)) {
    if (_cli.available()) {
      (void)_cli.read();
    } else {
      delay(5);
    }
  }
  _cli.stop();
  return ok;
}

bool TelegramService::sendViaUtb(const String& token, const String& chatId,
                                 const String& topicId, const String& text,
                                 int parseMode) {
  ensureBotToken(token);
  const char* pm = parseModeString(parseMode);
  // saniyo's UniversalTelegramBot fork supports topic_id as the 4th
  // arg; when topicId is empty we pass 0 which the fork treats as
  // "no topic".
  long topic_thread = topicId.isEmpty() ? 0L : topicId.toInt();
  if (topic_thread > 0) {
    return _bot.sendMessage(chatId, text, pm ? pm : "", topic_thread);
  }
  return _bot.sendMessage(chatId, text, pm ? pm : "");
}

void TelegramService::ensureBotToken(const String& token) {
  if (token == _botTokenInBot) return;
  _bot = UniversalTelegramBot(token.c_str(), _cli);
  _botTokenInBot = token;
}

// ─── log + status ───────────────────────────────────────────────────
void TelegramService::logLine(bool ok, const char* stage, const String& text) {
  TelegramLogEntry e;
  e.ts_s  = (uint32_t)(millis() / 1000);
  e.ok    = ok;
  e.stage = stage ? stage : "";
  e.text  = text;

  _state.chatLog.push_back(std::move(e));
  if (_state.chatLog.size() > TelegramSettings::LOG_MAX) {
    _state.chatLog.erase(_state.chatLog.begin());
  }

  _state.lastMessage  = text;
  _state.lastSendOk   = ok;
  _state.lastSendAt_s = (uint32_t)(millis() / 1000);
}

void TelegramService::updateStatusLabel() {
  if (!_state.enabled) {
    _state.statusLabel = "Disabled";
  } else if (_state.qSize > 0) {
    _state.statusLabel = String("Sending… (") + _state.qSize + " queued)";
  } else if (_state.lastSendAt_s > 0) {
    _state.statusLabel = _state.lastSendOk
        ? String("Last send OK at ") + _state.lastSendAt_s + "s"
        : String("Last send FAILED");
  } else {
    _state.statusLabel = "Idle";
  }
}

const char* TelegramService::parseModeString(int code) {
  switch (code) {
    case 1: return "Markdown";
    case 2: return "MarkdownV2";
    case 3: return "HTML";
    case 0:
    default: return nullptr;
  }
}

// ─── worker task ────────────────────────────────────────────────────
void TelegramService::taskTrampoline(void* pv) {
  static_cast<TelegramService*>(pv)->runWorker();
}

void TelegramService::runWorker() {
  TelegramQueuedMessage m;
  for (;;) {
    if (xQueueReceive(_q, &m, pdMS_TO_TICKS(50)) != pdPASS) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    const String token = !m.tokenOverride.isEmpty() ? m.tokenOverride : _state.botToken;
    const String chat  = !m.chatOverride.isEmpty()  ? m.chatOverride  : _state.chatId;
    const String topic = !m.topicOverride.isEmpty() ? m.topicOverride : _state.topicId;
    const int    pm    = (m.parseMode >= 0)         ? m.parseMode     : _state.parseMode;
    const bool   silent= m.silent || _state.silentDefault;

    bool ok = false;
    if (!_state.enabled || chat.isEmpty() || token.isEmpty()) {
      logLine(false, "skip",
              !_state.enabled ? "Bot disabled"
            : chat.isEmpty()  ? "No chat configured"
            :                   "No token configured");
    } else {
      ok = sendDirect(token, chat, topic, m.text, pm, silent);
      if (!ok) {
        // Different TLS path — saniyo's UTB fork can sometimes connect
        // when raw POST struggles with handshake. Properly checked
        // return value (no longer "true if didn't crash").
        ok = sendViaUtb(token, chat, topic, m.text, pm);
      }
      logLine(ok, ok ? "ok" : "fail", m.text);
      if (ok) _lastSendOkAt_s = (uint32_t)(millis() / 1000);
    }

    // Update per-subscription error counter on failure (sent already
    // optimistically incremented at enqueue time).
    if (m.subId != 0) {
      if (auto* rec = findSub(m.subId)) {
        if (!ok) rec->stats.errors++;
      }
    }

    _state.qSize = uxQueueMessagesWaiting(_q);
    updateStatusLabel();
    callUpdateHandlers("sta");

    vTaskDelay(pdMS_TO_TICKS(_state.sendIntervalMs));
  }
}
