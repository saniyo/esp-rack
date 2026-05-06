#include <PresenceService.h>

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

void PresenceService::mountWs(AsyncWebServer* server, SecurityManager* sm) {
  if (_ws || !server || !sm) return;
  _sm = sm;
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
  _ws->handleHandshake([sm](AsyncWebServerRequest* request) -> bool {
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
}

void PresenceService::setLogEnabled(bool on) {
  _logEnabled = on;
}

void PresenceService::appendLog(const String& line) {
  if (!_logEnabled || !_logFs || _logPath.length() == 0) return;

  // Simple size cap — if the file grows past _logMaxBytes we roll over by
  // truncating. Keeping it dumb: no rotation to .1/.2 — admin exports via
  // FileSystem page when they need history, and we don't fill flash.
  if (_logFs->exists(_logPath)) {
    File probe = _logFs->open(_logPath, "r");
    if (probe) {
      const size_t sz = probe.size();
      probe.close();
      if (_logMaxBytes > 0 && sz >= _logMaxBytes) {
        _logFs->remove(_logPath);
      }
    }
  }

  File f = _logFs->open(_logPath, "a");
  if (!f) return;
  f.print(millis());
  f.print('\t');
  f.println(line);
  f.close();
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
}
