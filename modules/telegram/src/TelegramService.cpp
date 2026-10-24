#include "TelegramService.h"
#include <ArduinoJson.h>
#include "TelegramCommand.h"
#include <WiFi.h>
#include <WebManager.h>
#include <WebFeatureSpec.h>
#include <web/Form.h>
#include <DeviceIdentity.h>

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
  _cmdMx = xSemaphoreCreateMutex();
  _inQ = xQueueCreate(4, sizeof(TelegramInbound*));
  // Queue holds POINTERS to heap-allocated TelegramQueuedMessage. We
  // can't memcpy the struct by value because it carries Arduino String
  // members — the FreeRTOS queue's bitwise copy duplicates the
  // String's internal buffer pointer but the source object's
  // destructor runs at scope exit and frees that buffer, leaving the
  // queued copy with a dangling pointer that the worker eventually
  // dereferences (manifested as "manual send works, scheduled
  // subscription sends silently corrupt or never deliver" — heap
  // reuse timing made the bug intermittent in early testing).
  _q = xQueueCreate(TEL_QUEUE_DEPTH, sizeof(TelegramQueuedMessage*));
  _subs.reserve(8);
}

TelegramService::~TelegramService() {
  if (_task) vTaskDelete(_task);
  if (_q) {
    // Drain any messages that didn't make it to the worker before we
    // tear down — without this their heap buffers leak.
    TelegramQueuedMessage* m = nullptr;
    while (xQueueReceive(_q, &m, 0) == pdPASS) {
      delete m;
    }
    vQueueDelete(_q);
  }
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
  // restRead/restUpdate omitted: registry derives /api/v1/telegram/form + /api/v1/telegram/state from spec.id.
  spec.wsPath     = TEL_WS_PATH;

  // Own-feature tabs leave restPath null — manifest emits the derived
  // form path (/api/v1/telegram/form) for them.
  WebTabSpec statusTab;
  statusTab.key      = "status";
  statusTab.title    = "Status";
  statusTab.postable = false;
  statusTab.live     = true;
  statusTab.auth     = WebAuthLevel::Admin;
  spec.tabs.push_back(statusTab);

  WebTabSpec subsTab;
  subsTab.key      = "subs";
  subsTab.title    = "Subscriptions";
  subsTab.postable = false;
  subsTab.auth     = WebAuthLevel::Admin;
  subsTab.order    = 15;
  spec.tabs.push_back(subsTab);

  WebTabSpec settingsTab;
  settingsTab.key      = "settings";
  settingsTab.title    = "Settings";
  settingsTab.postable = true;
  settingsTab.auth     = WebAuthLevel::Admin;
  settingsTab.order    = 20;
  spec.tabs.push_back(settingsTab);
  spec.staticSchema = true;  // the form never depends on the state

  // Wrap the static form reader in a lambda that appends the
  // Subscriptions sub-form. Static buildForm doesn't know about
  // subscriptions (and shouldn't — it's also used by ConfigDelegate
  // for the secret-key probe, which mustn't traverse runtime state).
  // The lambda renders Status + Settings via the static reader, then
  // adds a Subscriptions sub-form on top, populated from _subs.
  auto fullReader = [this](TelegramSettings& s, JsonObject& root) {
    TelegramSettings::buildForm(s, root);

    web::Form subs(root, "subs",
                   "Internal services streaming through this bot");
    subs.add(web::Table("subscriptions")
                 .col("name", "Service", "text")
                 .col("sent", "Sent", "number", "0")
                 .col("errors", "Errors", "number", "0")
                 .col("dropped", "Dropped", "number", "0")
                 .col("rate", "Rate (msg/min)", "number", "0")
                 .col("lastAt", "Last (s ago)", "number", "0")
                 .col("topic", "Topic ID", "text")
                 .col("enabled", "Enabled", "text")
                 .mode("replace").maxRows(32)
                 .icon("Cable").label("Active subscriptions"));
    JsonArray rows = subs.rowsOf("subscriptions");
    fillSubscriptionRows(rows);
  };

  // 8k REST buffer — measured form payload with empty log + no subs
  // is 1.8 KB; a fully-saturated 30-row log + handful of subs lands
  // around 5-6 KB worst case. Was 24576, which on ESP32-C3 with
  // active WiFi+TLS exceeds the max-contiguous-alloc window (typical
  // ~19 KB after Wi-Fi/lwIP/AsyncWebServer/mTLS populate). Old
  // failure mode: ArduinoJson silently left the doc uninitialised,
  // setLength() serialised the null variant as literal "null", UI
  // showed "Loading…" forever after Save. New behaviour: if 8K still
  // can't allocate under extreme pressure, HttpEndpoint's OOM guard
  // returns 503 → useRest surfaces a snackbar error rather than
  // permanently spinning. WS stays 8K (status+log only, no subs).
  //
  // Stays EXPLICIT 8192/8192 (not WEBFEATURE_BUFFER_AUTO): the boot
  // probe would measure an EMPTY chatLog (zero sends at boot) and an
  // empty _subs table (services subscribe after the probe), sizing the
  // buffer ~1.5 KB — then the 30-row log (~2.4-4.8 KB) plus subs rows
  // appearing at runtime would be silently dropped from both the REST
  // form and the WS pushes. 8192 covers the saturated-log worst case
  // (~5-6 KB) with margin.
  // /state and WS: the settings echo, the log and the subscription rows, so
  // the static schema gets every value from here.
  auto stateReader = [this](TelegramSettings& s, JsonObject& root) {
    TelegramSettings::staRead(s, root);
    fillSubscriptionRows(root.createNestedArray("subscriptions"));
  };

  _feature = web->registerFeature<TelegramSettings>(
      std::move(spec), this,
      fullReader,                 TelegramSettings::upd,
      stateReader,                TelegramSettings::staUpd,
      8192, 8192);
}

void TelegramService::begin() {
  (void)_cfg.ensureLoaded();
  _cli.setInsecure();
  ensureBotToken(_state.botToken);

  // ALWAYS overwrite. deviceLabel is runtime-derived, not persisted —
  // single source of truth for device identity across cert / MQTT /
  // mothership / AutoUpdate / Telegram. Any value an older firmware
  // version may have written to disk is replaced here.
  _state.deviceLabel = DeviceIdentity::canonical();

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
    if (_state.enabled && !_task) startTask();  // enabled at runtime: no reboot
    ensureBotToken(_state.botToken);
    updateStatusLabel();
    _cfg.saveIfChanged(origin);
    // No broadcastWs here (LIB-FIX-009): the feature's WS binding pushes on
    // every propagated update, after this handler has refreshed state.
  }, false);

  // Static stack — the 6 KB lives in BSS (not heap), so it doesn't
  // appear as a permanent contiguous-heap hole. See Phase 1 of
  // docs/plans/heap-baseline-fix.md — measured ~6 KB max_alloc gain.
  //
  // Gate: only spawn the task when the operator has enabled Telegram
  // in config. Idle measurement showed TelTask uses only ~400 B of
  // its 6 KB stack when disabled (the polling loop just sees no work
  // and sleeps), so we wasted a whole task slot + scheduler overhead
  // for zero benefit. With this gate, an unconfigured device boots
  // with one less task in the table and saves ~6 KB BSS reserved for
  // the stack ALSO becomes claim-only-when-needed: the static arrays
  // still occupy BSS (predictable layout), but no task is actually
  // running against them. Future improvement: lazily spawn the task
  // on first save where enabled=true (no reboot needed) — see
  // docs/plans/disable-inactive-tasks.md.
  if (_state.enabled) {
    startTask();
  } else {
    log_i("[tel] task NOT started — Telegram disabled in config");
  }

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

  // Heap-owned message — see queue comment in ctor for why we can't
  // pass by value. delete-on-consume in the worker (and on dtor for
  // any remaining queue entries) covers the lifetime.
  auto* m = new TelegramQueuedMessage();
  m->subId          = rec->id;
  m->parseMode      = opt.parseMode;
  m->silent         = opt.silent;
  m->chatOverride   = !explicitTo.chatId.isEmpty()  ? explicitTo.chatId  : rec->cfg.defaultChatId;
  m->topicOverride  = !explicitTo.topicId.isEmpty() ? explicitTo.topicId : rec->cfg.defaultTopicId;

  // Apply tag prefix. Empty tagPrefix → use "<name>: " auto-prefix
  // (Markdown-safe — brackets [...] would be interpreted as the
  // start of a `[text](url)` link in parseMode=Markdown).
  String prefix = rec->cfg.tagPrefix;
  if (prefix.length() == 0) {
    prefix = rec->name + String(": ");
  }
  // Device-label outer prefix (mirrors MQTT clientId — same canonical
  // ID identifies the device on either transport). Empty deviceLabel
  // skips the prefix entirely so operator can opt out per-device. Built
  // as a separate leading line so Markdown link-syntax mis-parsing of
  // brackets doesn't apply to the message body below.
  String labelPrefix;
  if (_state.deviceLabel.length() > 0) {
    labelPrefix = String("[") + _state.deviceLabel + "] ";
  }
  m->text = labelPrefix + prefix + text;

  if (xQueueSend(_q, &m, 0) != pdTRUE) {
    delete m;
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

  auto* m = new TelegramQueuedMessage();
  m->subId          = 0;  // legacy path — no subscription accounting
  m->chatOverride   = chatId;
  m->topicOverride  = topicId;
  m->tokenOverride  = tokenOverride;
  // Device-label prefix on the legacy path too — same justification as
  // doSend above. Operator who clears deviceLabel disables the prefix
  // on both code paths uniformly.
  if (_state.deviceLabel.length() > 0) {
    m->text = String("[") + _state.deviceLabel + "] " + text;
  } else {
    m->text = text;
  }
  m->parseMode      = opt.parseMode;
  m->silent         = opt.silent;

  if (xQueueSend(_q, &m, 0) != pdTRUE) {
    delete m;
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
  // ASCII only — non-ASCII glyphs (ellipsis, middle dot) in source
  // round-trip badly when the .cpp is saved as anything other than
  // UTF-8 (Windows cp1252 default sometimes lands here), producing
  // standalone invalid UTF-8 bytes in WS text frames. Browser then
  // rejects every frame with "Could not decode a text frame as UTF-8"
  // and the LiveIndicator goes into a reconnect loop.
  if (!_state.enabled) {
    _state.statusLabel = "Disabled";
  } else if (_state.qSize > 0) {
    _state.statusLabel = String("Sending... (") + _state.qSize + " queued)";
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
void TelegramService::startTask() {
  if (_task) return;
  constexpr uint32_t kStackWords = 6144 / sizeof(StackType_t);
  static StackType_t  s_telStack[kStackWords];
  static StaticTask_t s_telBlock;
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
  _task = xTaskCreateStatic(taskTrampoline, "TelTask", kStackWords,
                            this, 1, s_telStack, &s_telBlock);
#else
  _task = xTaskCreateStaticPinnedToCore(
      taskTrampoline, "TelTask", kStackWords, this, 1,
      s_telStack, &s_telBlock, 1);
#endif
  log_i("[tel] task started (enabled=true)");
}

void TelegramService::taskTrampoline(void* pv) {
  static_cast<TelegramService*>(pv)->runWorker();
}

void TelegramService::runWorker() {
  constexpr uint8_t kSendTries = 3;
  constexpr uint32_t kRetryPauseMs = 5000;
  TelegramQueuedMessage* m = nullptr;
  for (;;) {
    // ask for commands every cmd_poll_s while any is registered.
    if (_state.enabled && !_state.botToken.isEmpty() && WiFi.isConnected() && !_cmds.empty() &&
        static_cast<uint32_t>(millis() - _lastPollMs) >= static_cast<uint32_t>(_state.cmdPollS) * 1000u) {
      _lastPollMs = millis();
      pollCommands(_state.botToken);
    }
    // Offline: leave the queue alone, so what was queued meanwhile (a boot
    // report right after a crash, say) goes out once the network is back.
    if (_state.enabled && !WiFi.isConnected()) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (xQueueReceive(_q, &m, pdMS_TO_TICKS(50)) != pdPASS || !m) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    const String token = !m->tokenOverride.isEmpty() ? m->tokenOverride : _state.botToken;
    const String chat  = !m->chatOverride.isEmpty()  ? m->chatOverride  : _state.chatId;
    const String topic = !m->topicOverride.isEmpty() ? m->topicOverride : _state.topicId;
    const int    pm    = (m->parseMode >= 0)         ? m->parseMode     : _state.parseMode;
    const bool   silent= m->silent || _state.silentDefault;

    bool ok = false;
    if (!_state.enabled || chat.isEmpty() || token.isEmpty()) {
      logLine(false, "skip",
              !_state.enabled ? "Bot disabled"
            : chat.isEmpty()  ? "No chat configured"
            :                   "No token configured");
    } else {
      ok = sendDirect(token, chat, topic, m->text, pm, silent);
      if (!ok) {
        // UTB fallback — different TLS path that can sometimes
        // succeed when raw POST struggles with handshake.
        ok = sendViaUtb(token, chat, topic, m->text, pm);
      }
      if (!ok && ++m->tries < kSendTries && xQueueSendToFront(_q, &m, 0) == pdTRUE) {
        // Back to the head of the queue, order kept: tried again after a
        // pause, or once the network is back.
        logLine(false, "retry", m->text);
        m = nullptr;
        vTaskDelay(pdMS_TO_TICKS(kRetryPauseMs));
        continue;
      }
      logLine(ok, ok ? "ok" : "fail", m->text);
      if (ok) _lastSendOkAt_s = (uint32_t)(millis() / 1000);
    }

    if (m->subId != 0) {
      if (auto* rec = findSub(m->subId)) {
        if (!ok) rec->stats.errors++;
      }
    }

    delete m;
    m = nullptr;

    _state.qSize = uxQueueMessagesWaiting(_q);
    updateStatusLabel();
    callUpdateHandlers("sta");

    vTaskDelay(pdMS_TO_TICKS(_state.sendIntervalMs));
  }
}

// ── Commands ─────────────────────────────────────────────────

namespace {
// Command replies go out as plain text: with the bot's default Markdown a
// lone "_" (RELAY_STUCK) makes Telegram reject the whole reply.
const TelegramSendOptions kCommandReply{0 /*parse mode: none*/, false};
}  // namespace

bool TelegramService::doOnCommand(uint32_t subId, const char* cmd, const char* help, TelegramCommandHandler h,
                                  bool adminOnly) {
  if (!cmd || !*cmd || !h || !findSub(subId)) return false;
  String name = cmd[0] == '/' ? String(cmd + 1) : String(cmd);
  name.toLowerCase();
  xSemaphoreTake(_cmdMx, portMAX_DELAY);
  bool ok = _cmds.size() < kMaxCommands;
  for (const auto& c : _cmds) ok = ok && c.cmd != name;
  if (ok) {
    _cmds.push_back(TgCommand{subId, name, help ? String(help) : String(), std::move(h), adminOnly});
    ++_cmdsRev;
  }
  xSemaphoreGive(_cmdMx);
  return ok;
}

void TelegramService::loop() {
  TelegramInbound* in = nullptr;
  while (_inQ && xQueueReceive(_inQ, &in, 0) == pdPASS && in) {
    TelegramCommandHandler h;
    bool adminOnly = false;
    xSemaphoreTake(_cmdMx, portMAX_DELAY);
    for (const auto& c : _cmds) {
      if (c.cmd == in->command) {
        h = c.h;
        adminOnly = c.adminOnly;
        break;
      }
    }
    xSemaphoreGive(_cmdMx);
    if (h && (!adminOnly || in->admin)) {
      const String reply = h(*in);
      if (!reply.isEmpty()) {
        enqueueMessage(in->chatId, in->topicId ? String(in->topicId) : String(), reply, kCommandReply, String());
      }
    }
    delete in;
  }
}

bool TelegramService::postJson(const String& token, const char* method, const String& payload) {
  const char* host = "api.telegram.org";
  _cli.stop();
  _cli.setHandshakeTimeout(8);
  _cli.setTimeout(5000);
  if (!_cli.connect(host, 443, 5000)) return false;
  _cli.print(String("POST /bot") + token + "/" + method + " HTTP/1.0\r\nHost: " + host +
             "\r\nContent-Type: application/json\r\nContent-Length: " + String(payload.length()) + "\r\n\r\n" +
             payload);
  const bool ok = _cli.readStringUntil('\n').indexOf(" 200 ") >= 0;
  _cli.stop();
  return ok;
}

void TelegramService::publishCommands(const String& token) {
  String body = "{\"commands\":[";
  uint32_t rev;
  xSemaphoreTake(_cmdMx, portMAX_DELAY);
  rev = _cmdsRev;
  for (size_t i = 0; i < _cmds.size(); ++i) {
    StaticJsonDocument<256> one;
    one["command"] = _cmds[i].cmd;
    one["description"] = _cmds[i].help.isEmpty() ? _cmds[i].cmd : _cmds[i].help;
    if (i) body += ',';
    serializeJson(one, body);
  }
  xSemaphoreGive(_cmdMx);
  body += "]}";
  if (postJson(token, "setMyCommands", body)) _cmdsPublished = rev;
}

void TelegramService::pollCommands(const String& token) {
  const char* host = "api.telegram.org";
  _cli.stop();
  _cli.setHandshakeTimeout(8);
  _cli.setTimeout(5000);
  if (!_cli.connect(host, 443, 5000)) return;
  // Not primed yet: only the newest update, to skip the backlog.
  const String offset = _updOffset ? String(_updOffset) : String("-1");
  _cli.print(String("GET /bot") + token + "/getUpdates?timeout=0&limit=4&offset=" + offset +
             "&allowed_updates=%5B%22message%22%5D HTTP/1.0\r\nHost: " + host + "\r\n\r\n");
  const bool httpOk = _cli.readStringUntil('\n').indexOf(" 200 ") >= 0;
  while (httpOk && _cli.connected()) {  // headers
    const String line = _cli.readStringUntil('\n');
    if (line.length() <= 1) break;
  }
  StaticJsonDocument<256> filter;
  JsonObject fu = filter["result"].createNestedObject();
  fu["update_id"] = true;
  JsonObject fm = fu.createNestedObject("message");
  fm["text"] = true;
  fm["message_thread_id"] = true;
  fm["chat"]["id"] = true;
  fm["from"]["id"] = true;
  DynamicJsonDocument doc(2048);
  const bool parsed = httpOk && !deserializeJson(doc, _cli, DeserializationOption::Filter(filter));
  _cli.stop();
  if (!parsed) return;

  const bool primed = _updOffset != 0;
  for (JsonObject u : doc["result"].as<JsonArray>()) {
    const long long id = u["update_id"] | 0LL;
    if (id + 1 > _updOffset) _updOffset = id + 1;
    if (!primed) continue;  // the backlog
    JsonObject msg = u["message"];
    const tgcmd::Parsed p = tgcmd::parse(msg["text"] | "");
    if (!p.ok) continue;
    char chat[24], from[24];
    snprintf(chat, sizeof(chat), "%lld", msg["chat"]["id"] | 0LL);
    snprintf(from, sizeof(from), "%lld", msg["from"]["id"] | 0LL);
    const int32_t topic = msg["message_thread_id"] | 0;

    bool known = false, adminCmd = false;
    xSemaphoreTake(_cmdMx, portMAX_DELAY);
    for (const auto& c : _cmds) {
      if (c.cmd == p.command.c_str()) {
        known = true;
        adminCmd = c.adminOnly;
        break;
      }
    }
    xSemaphoreGive(_cmdMx);
    const bool admin = tgcmd::inList(_state.adminUserIds.c_str(), from);
    const tgcmd::Verdict v = tgcmd::decide(
        tgcmd::chatAllowed(_state.allowedChatIds.c_str(), chat, topic, _state.chatId.c_str()), known, adminCmd, admin);
    if (v == tgcmd::Verdict::Ignore) {
      ++_cmdIgnored;
      continue;
    }
    if (v == tgcmd::Verdict::AdminOnly) {
      // At most once a minute per user, so nobody can flood the send queue.
      AdminNag* slot = &_nags[0];
      for (auto& n : _nags) {
        if (n.fromId == from) { slot = &n; break; }
        if (n.at < slot->at) slot = &n;
      }
      const uint32_t now = millis();
      if (slot->fromId == from && now - slot->at < 60000u) continue;
      slot->fromId = from;
      slot->at = now;
      enqueueMessage(chat, topic ? String(topic) : String(), "Admin only.", kCommandReply, String());
      continue;
    }
    auto* in = new TelegramInbound{chat, topic, from, p.command.c_str(), p.args.c_str(), admin};
    if (xQueueSend(_inQ, &in, 0) != pdPASS) {
      delete in;
      ++_cmdDropped;
    }
  }
  if (_updOffset && _cmdsPublished != _cmdsRev) publishCommands(token);
}

void TelegramService::fillSubscriptionRows(JsonArray rows) const {
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
}
