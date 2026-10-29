#include <PresenceService.h>
#include <power/FlashWriter.h>
#include <power/WriteGate.h>

namespace {
// The queue lock: held for a deque operation only, so a short wait.
constexpr TickType_t kLogLockWait = pdMS_TO_TICKS(50);
// A log line is written in one piece: no 4 KB stdio buffer per append.
constexpr size_t kLogFileBuf = 128;
}  // namespace

PresenceService::PresenceService() {}

// --------------------------------------------------------------------------
// WebSocket /ws/presence wiring
// --------------------------------------------------------------------------

// Strip the SPA origin prefix from a raw page string — the frontend always
// sends pathname+search, trim to something the admin view can fit.
static String clipPage(const String& raw) {
  if (raw.length() <= 80) return raw;
  return raw.substring(0, 77) + "...";
}

void PresenceService::mountWs(AsyncWebServer* server, SecurityResolver resolveSecurity) {
  if (_ws || !server || !resolveSecurity) return;
  _resolveSecurity = std::move(resolveSecurity);
  _ws = new AsyncWebSocket(PRESENCE_WS_PATH);

  _ws->onEvent([this](AsyncWebSocket* /*srv*/, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (!client) return;
    const uint32_t cid = client->id();

    switch (type) {
      case WS_EVT_CONNECT:
        // Wait for hello — we don't know clientKey yet. bindWsTransport
        // happens when a hello frame arrives.
        break;

      case WS_EVT_DISCONNECT:
      case WS_EVT_ERROR:
        unbindWsTransport(cid);
        break;

      case WS_EVT_DATA: {
        AwsFrameInfo* info = reinterpret_cast<AwsFrameInfo*>(arg);
        if (!info || !info->final || info->index != 0 || info->opcode != WS_TEXT) break;
        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, data, len)) break;
        const char* mtype = doc["type"] | "";
        if (!mtype || !*mtype) break;

        const String ip = client->remoteIP().toString();

        if (strcmp(mtype, "hello") == 0) {
          const String key  = doc["clientKey"] | "";
          const String page = doc["page"]      | "";
          const String ua   = doc["userAgent"] | "";
          if (key.length()) {
            touch(key, page, ip, ua, /*viaWs=*/true);
            bindWsTransport(key, cid);
            appendLog(String("CONNECT\t") + key + "\t" + ip + "\t" + page);
          }
        } else if (strcmp(mtype, "page") == 0) {
          const String page = doc["page"]      | "";
          // key isn't supplied in page frames — we look it up by current
          // transport id instead. That also protects against spoofing
          // (can't claim another tab's key).
          for (auto& kv : _clients) {
            if (kv.second.presenceTransportId == cid) {
              if (page.length() && page != kv.second.currentPage) {
                PageEntry e{page, millis()};
                kv.second.pageHistory.push_back(std::move(e));
                while (kv.second.pageHistory.size() > PAGE_HISTORY_MAX) {
                  kv.second.pageHistory.pop_front();
                }
                kv.second.currentPage = page;
                appendLog(String("PAGE\t") + kv.second.clientKey + "\t" + page);
              }
              kv.second.lastWsAtMs = millis();
              break;
            }
          }
        }
      } break;

      default:
        break;
    }
  });

  // Auth predicate: any authenticated user can open presence WS — it's the
  // SPA's identity channel, not a privileged endpoint. Admin-only pages
  // and sensitive APIs stay IS_ADMIN through their own wraps.
  _ws->handleHandshake([this](AsyncWebServerRequest* request) -> bool {
    SecurityManager* sm = _resolveSecurity ? _resolveSecurity() : nullptr;
    if (!sm) return true;
    Authentication a = sm->authenticateRequest(request);
    return AuthenticationPredicates::IS_AUTHENTICATED(a);
  });

  server->addHandler(_ws);
}

// --------------------------------------------------------------------------
// SD log sink
// --------------------------------------------------------------------------
void PresenceService::setLogSink(fs::FS* fs, const char* path, size_t maxBytes) {
  _logFs       = fs;
  _logPath     = path ? path : "/logs/presence.log";
  _logMaxBytes = maxBytes;
  // Setup: the queue lock and the FlashWriter job for the appends the
  // write gate puts off. Without them the appends go at once, as before.
  if (!_logMx) _logMx = xSemaphoreCreateMutex();
  if (_logJob < 0 && _logMx) {
    _logJob = ESPRack::FlashWriter::add("presence", [this]() -> uint32_t { return drainLog(0); });
    if (_logJob < 0) log_w("[presence] no FlashWriter job left: log appends go at once");
  }
}

void PresenceService::setLogEnabled(bool on) {
  _logEnabled = on;
}

bool PresenceService::claimLogWriter() {
  portENTER_CRITICAL(&_logMux);
  const bool ok = !_logWriting;
  _logWriting = true;
  portEXIT_CRITICAL(&_logMux);
  return ok;
}

void PresenceService::releaseLogWriter() {
  portENTER_CRITICAL(&_logMux);
  _logWriting = false;
  portEXIT_CRITICAL(&_logMux);
}

void PresenceService::countLogLost(size_t lines) {
  portENTER_CRITICAL(&_logMux);
  _logLost += uint32_t(lines);
  portEXIT_CRITICAL(&_logMux);
}

// Simple size cap — if the file grows past _logMaxBytes we roll over by
// truncating. Keeping it dumb: no rotation to .1/.2 — admin exports via
// FileSystem page when they need history, and we don't fill flash.
bool PresenceService::logOverCap() const {
  if (_logMaxBytes == 0 || !_logFs->exists(_logPath)) return false;
  File probe = _logFs->open(_logPath, "r");  // size only: no I/O, no stdio buffer
  if (!probe) return false;
  const size_t sz = probe.size();
  probe.close();
  return sz >= _logMaxBytes;
}

void PresenceService::logRoll() { _logFs->remove(_logPath); }

bool PresenceService::logAppend(const char* data, size_t len) {
  File f = _logFs->open(_logPath, "a");
  if (!f) return false;
  f.setBufferSize(kLogFileBuf);  // before the first write: the buffer comes then
  const size_t n = f.write(reinterpret_cast<const uint8_t*>(data), len);
  f.close();
  return n == len;
}

void PresenceService::appendLog(const String& line) {
  if (!_logEnabled || !_logFs || _logPath.length() == 0) return;

  String row;
  row.reserve(line.length() + 16);
  row += millis();
  row += '\t';
  row += line;
  row += "\r\n";

  if (_logJob < 0) {  // no queue / job: at once, as without the gate
    if (logOverCap()) logRoll();
    (void)logAppend(row.c_str(), row.length());
    return;
  }

  // At once when nothing is queued and nobody is writing, else behind the
  // queued lines (the order stays; one writer of the file at a time).
  if (xSemaphoreTake(_logMx, kLogLockWait) != pdTRUE) {
    countLogLost(1);
    return;
  }
  const bool direct = _logQueue.empty() && claimLogWriter();
  if (!direct) _logQueue.push(std::move(row));
  xSemaphoreGive(_logMx);
  if (!direct) {
    ESPRack::FlashWriter::kick(_logJob);
    return;
  }

  // async_tcp: a gate pass, never a wait. One step: the append, or the
  // roll-over (then the line goes to the job, a step of its own).
  bool queue = true;
  {
    ESPRack::WritePass pass(ESPRack::WriteGate::now());
    if (!pass.ok()) {
      ESPRack::WriteGate::noteDeferred();
    } else if (logOverCap()) {
      logRoll();
    } else {
      if (!logAppend(row.c_str(), row.length())) countLogLost(1);
      queue = false;
    }
  }
  if (queue) {
    if (xSemaphoreTake(_logMx, kLogLockWait) == pdTRUE) {
      _logQueue.push(std::move(row));
      xSemaphoreGive(_logMx);
    } else {
      countLogLost(1);
      queue = false;
    }
  }
  releaseLogWriter();
  if (queue) ESPRack::FlashWriter::kick(_logJob);
}

uint32_t PresenceService::drainLog(uint32_t boundMs) {
  if (!_logFs || !_logMx) return ESPRack::FlashWriter::kDone;
  if (!claimLogWriter()) return 10;  // an append on async_tcp is at it: look again
  // One wait clock for the run, so maxWaitMs bounds it, not each batch.
  const uint32_t since = ESPRack::WriteGate::now();
  uint32_t r = ESPRack::FlashWriter::kDone;
  for (;;) {
    if (xSemaphoreTake(_logMx, kLogLockWait) != pdTRUE) {
      r = 10;
      break;
    }
    const bool empty = _logQueue.empty();
    xSemaphoreGive(_logMx);
    if (empty) break;

    ESPRack::WriteSlot slot(boundMs, nullptr, since);
    if (!slot.ok()) {  // PowerGuard paused writes: the lines stay queued
      r = ESPRack::FlashWriter::kOnPower;
      break;
    }
    if (logOverCap()) {
      logRoll();
      if (!slot.again()) {
        r = ESPRack::FlashWriter::kOnPower;
        break;
      }
    }
    // What this slot writes, taken now: lines that come meanwhile wait in
    // the queue (bounded), not in a batch that could be lost.
    String batch;
    size_t n = 0;
    if (xSemaphoreTake(_logMx, kLogLockWait) == pdTRUE) {
      batch.reserve(LOG_BATCH_BYTES < _logQueue.bytes() ? LOG_BATCH_BYTES : _logQueue.bytes());
      n = _logQueue.take(batch, LOG_BATCH_BYTES);
      xSemaphoreGive(_logMx);
    } else {
      r = 10;
      break;
    }
    if (n && !logAppend(batch.c_str(), batch.length())) {
      log_w("[presence] log append failed: %u lines lost", unsigned(n));
      countLogLost(n);
    }
  }
  releaseLogWriter();
  return r;
}

void PresenceService::flushLog(uint32_t boundMs) {
  if (_logJob < 0) return;
  (void)drainLog(boundMs);  // the FlashWriter task at it: it finishes them
}

size_t PresenceService::logPending() const {
  if (!_logMx || xSemaphoreTake(_logMx, kLogLockWait) != pdTRUE) return 0;
  const size_t n = _logQueue.lines();
  xSemaphoreGive(_logMx);
  return n;
}

uint32_t PresenceService::logDropped() const {
  uint32_t n = 0;
  if (_logMx && xSemaphoreTake(_logMx, kLogLockWait) == pdTRUE) {
    n = _logQueue.dropped();
    xSemaphoreGive(_logMx);
  }
  portENTER_CRITICAL(&_logMux);
  n += _logLost;
  portEXIT_CRITICAL(&_logMux);
  return n;
}

PresenceService::ClientPresence&
PresenceService::upsertLocked(const String& clientKey,
                              const String& remoteIp,
                              const String& userAgent) {
  auto it = _clients.find(clientKey);
  if (it == _clients.end()) {
    ClientPresence cp;
    cp.clientKey      = clientKey;
    cp.remoteIp       = remoteIp;
    cp.userAgent      = userAgent;
    cp.firstSeenAtMs  = millis();
    it = _clients.emplace(clientKey, std::move(cp)).first;
  } else {
    // Update IP / UA opportunistically — a proxy change or a UA swap (rare)
    // should be reflected without erasing the historical record.
    if (remoteIp.length())  it->second.remoteIp  = remoteIp;
    if (userAgent.length()) it->second.userAgent = userAgent;
  }
  return it->second;
}

void PresenceService::touch(const String& clientKey,
                            const String& page,
                            const String& remoteIp,
                            const String& userAgent,
                            bool viaWs) {
  if (clientKey.length() == 0) return;

  ClientPresence& cp = upsertLocked(clientKey, remoteIp, userAgent);
  const uint32_t now = millis();
  if (viaWs) {
    cp.lastWsAtMs = now;
    cp.zombieWsUntilMs = 0;  // active WS activity clears any zombie marker
  } else {
    cp.lastRestAtMs = now;
  }

  if (page.length() && page != cp.currentPage) {
    // Record the transition — ring-buffer capped to PAGE_HISTORY_MAX so
    // admin can see the navigation trail without growing indefinitely.
    PageEntry entry{page, now};
    cp.pageHistory.push_back(std::move(entry));
    while (cp.pageHistory.size() > PAGE_HISTORY_MAX) {
      cp.pageHistory.pop_front();
    }
    cp.currentPage = page;
  }
}

void PresenceService::touchFromRequest(AsyncWebServerRequest* request) {
  if (!request) return;

  const AsyncWebHeader* keyHdr = request->getHeader("X-Client-Key");
  if (!keyHdr) return;  // unknown client — nothing to upsert
  const String clientKey = keyHdr->value();
  if (clientKey.length() == 0) return;

  const AsyncWebHeader* pageHdr = request->getHeader("X-Current-Page");
  const String page = pageHdr ? pageHdr->value() : String();

  const AsyncWebHeader* uaHdr   = request->getHeader("User-Agent");
  const String ua = uaHdr ? uaHdr->value() : String();

  const String ip = request->client() ? request->client()->remoteIP().toString() : String();

  touch(clientKey, page, ip, ua, /*viaWs=*/false);
}

void PresenceService::bindWsTransport(const String& clientKey, uint32_t transportId) {
  if (clientKey.length() == 0) return;
  auto it = _clients.find(clientKey);
  if (it == _clients.end()) return;
  it->second.presenceTransportId = transportId;
  it->second.lastWsAtMs = millis();
  it->second.zombieWsUntilMs = 0;
}

void PresenceService::unbindWsTransport(uint32_t transportId, uint32_t zombieTtlMs) {
  if (transportId == 0) return;
  for (auto& kv : _clients) {
    if (kv.second.presenceTransportId == transportId) {
      kv.second.presenceTransportId = 0;
      kv.second.zombieWsUntilMs     = millis() + zombieTtlMs;
      appendLog(String("DISCONNECT\t") + kv.second.clientKey);
      break;
    }
  }
}

void PresenceService::purgeStale() {
  const uint32_t now = millis();
  for (auto it = _clients.begin(); it != _clients.end();) {
    // Keep anyone with an active WS transport. Anyone in the zombie-WS
    // window stays as well — they might come back and resume. Evict only
    // pure REST-silent clients older than REST_IDLE_EVICT_MS.
    const bool wsActive  = it->second.presenceTransportId != 0;
    const bool wsZombie  = it->second.zombieWsUntilMs > now;
    const bool restStale =
        it->second.lastRestAtMs == 0 ||
        (uint32_t)(now - it->second.lastRestAtMs) > REST_IDLE_EVICT_MS;
    if (!wsActive && !wsZombie && restStale) {
      it = _clients.erase(it);
    } else {
      ++it;
    }
  }
}

static const char* presenceStatusFor(const PresenceService::ClientPresence& c,
                                     uint32_t now) {
  if (c.presenceTransportId != 0) return "Online";
  if (c.zombieWsUntilMs > now)    return "Reconnecting";
  if (c.lastRestAtMs != 0 &&
      (uint32_t)(now - c.lastRestAtMs) < 60UL * 1000UL) {
    return "REST-only";
  }
  return "Idle";
}

void PresenceService::fillStats(JsonObject& root) const {
  const uint32_t now = millis();
  auto sinceSec = [now](uint32_t ts) -> uint32_t {
    if (ts == 0) return 0;
    return (uint32_t)((now - ts) / 1000UL);
  };

  JsonArray arr = root.createNestedArray("clients");
  for (const auto& kv : _clients) {
    const ClientPresence& c = kv.second;
    JsonObject j = arr.createNestedObject();
    j["key"]            = c.clientKey;
    j["ip"]             = c.remoteIp;
    j["ua"]             = c.userAgent;
    j["first_seen_s"]   = sinceSec(c.firstSeenAtMs);
    j["last_rest_s"]    = sinceSec(c.lastRestAtMs);
    j["last_ws_s"]      = sinceSec(c.lastWsAtMs);
    j["current_page"]   = c.currentPage;
    j["status"]         = presenceStatusFor(c, now);

    // Compact join of page history for the table cell — the admin can still
    // see each transition; if the full list were ever needed, add a
    // per-client drilldown endpoint.
    String joined;
    for (size_t i = 0; i < c.pageHistory.size(); ++i) {
      if (i) joined += " > ";
      joined += c.pageHistory[i].page;
    }
    j["page_history"]   = joined;
  }
  root["total_presence"] = _clients.size();
  if (_logFs) {  // the log sink: lines waiting for the write gate, lines lost
    root["log_pending"] = logPending();
    root["log_dropped"] = logDropped();
  }
}
