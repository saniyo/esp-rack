#pragma once
#ifndef PresenceService_h
#define PresenceService_h

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <FS.h>
#include <SecurityManager.h>

#include <deque>
#include <map>

#define PRESENCE_WS_PATH "/ws/presence"

// Central registry of every authenticated SPA tab. Fed from two channels:
//   1. REST (primary)   — touchFromRequest() reads X-Client-Key and
//                         X-Current-Page on every API call and upserts
//                         a ClientPresence.
//   2. WS presence      — optional /ws/presence stream for real-time
//                         navigation events (joined later in E2).
//
// Every SPA tab always exercises (1) at least once on load (manifest fetch)
// so the registry is populated even when the user sits on a page without
// any live-WS feature. The /ws/presence channel layers realtime status on
// top: "Online" when WS is up, "REST-only" when just REST is arriving,
// "Offline" after the REST-silence TTL elapses.
class PresenceService {
 public:
  struct PageEntry {
    String   page;
    uint32_t ts;
  };

  struct ClientPresence {
    String   clientKey;
    String   remoteIp;
    String   userAgent;
    uint32_t firstSeenAtMs{0};
    uint32_t lastRestAtMs{0};   // last REST touch (always updated from hook)
    uint32_t lastWsAtMs{0};     // last presence-WS event (populated in E2)
    String   currentPage;
    std::deque<PageEntry> pageHistory;  // capped at PAGE_HISTORY_MAX entries
    uint32_t presenceTransportId{0};    // /ws/presence WS id when connected
    uint32_t zombieWsUntilMs{0};        // if >0, WS is zombie; still lives via REST
  };

  static constexpr size_t   PAGE_HISTORY_MAX = 10;
  // REST-only tab silence window — longer than WS zombie TTL because REST
  // traffic is naturally sporadic (no 10 Hz stream forcing recency).
  static constexpr uint32_t REST_IDLE_EVICT_MS = 10UL * 60UL * 1000UL;

  PresenceService();

  // Wire the /ws/presence WebSocket. Safe to call once during ESPReact ctor;
  // subsequent calls are no-ops. The WS accepts `hello` and `page` frames
  // described in the header comment above.
  void mountWs(AsyncWebServer* server, SecurityManager* sm);

  // ==== SD log sink (toggleable) =========================================
  // When enabled, significant presence events (connect, disconnect, page
  // change) are appended to a rolling log file on the configured FS. Kept
  // explicitly off-by-default: a busy SPA can produce many rows per minute
  // and log writes go through async flash I/O. Admin toggles via UI; the
  // setter is idempotent and doesn't retroactively flush buffered events.
  void setLogSink(fs::FS* fs, const char* path = "/logs/presence.log",
                  size_t maxBytes = 64UL * 1024UL);
  void setLogEnabled(bool on);
  bool logEnabled() const { return _logEnabled; }

  // Hook into every secured REST handler (see SecuritySettingsService::
  // wrapRequest / wrapCallback): idempotent upsert keyed by X-Client-Key.
  // Missing header → no-op (preserves backward compatibility for any API
  // client that hasn't been updated yet).
  void touchFromRequest(AsyncWebServerRequest* request);

  // Explicit form used by the presence-WS path and by unit tests.
  void touch(const String& clientKey,
             const String& page,
             const String& remoteIp,
             const String& userAgent,
             bool viaWs = false);

  // Register/clear the presence-WS transport id that currently owns a given
  // client. Called from PresenceService::onWsEvent in E2.
  void bindWsTransport(const String& clientKey, uint32_t transportId);
  void unbindWsTransport(uint32_t transportId, uint32_t zombieTtlMs = 60UL * 1000UL);

  // Periodic cleanup — evicts clients that have neither REST touches in
  // REST_IDLE_EVICT_MS nor an active WS binding. Call from a ticker or
  // main-loop poll with a coarse cadence (every few seconds is enough).
  void purgeStale();

  // Snapshot for the admin Sockets tab (By Client view).
  void fillStats(JsonObject& root) const;

  size_t clientCount() const { return _clients.size(); }

 private:
  // Core upsert helper used by both touch() and touchFromRequest().
  ClientPresence& upsertLocked(const String& clientKey,
                               const String& remoteIp,
                               const String& userAgent);

  // Append a single log line to the rolling file sink if enabled. No-op if
  // either sink is unconfigured or toggle is off.
  void appendLog(const String& line);

  std::map<String, ClientPresence> _clients;  // keyed by clientKey

  AsyncWebSocket* _ws{nullptr};
  SecurityManager* _sm{nullptr};

  // SD/FS log sink state
  fs::FS*      _logFs{nullptr};
  String       _logPath;
  size_t       _logMaxBytes{0};
  bool         _logEnabled{false};
};

#endif  // PresenceService_h
