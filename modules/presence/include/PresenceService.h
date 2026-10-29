#pragma once
#ifndef PresenceService_h
#define PresenceService_h

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <FS.h>
#include <SecurityManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <deque>
#include <functional>
#include <map>

#include "PresenceLogQueue.h"

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
  //
  // The SecurityManager is RESOLVED PER HANDSHAKE (LIB-FIX-005): presence
  // installs before SecurityModule (security requires presence), so a
  // pointer captured at install time is App's NullSecurityManager, which
  // authenticates everyone as admin. The resolver returns whatever manager
  // the App holds when the browser connects.
  using SecurityResolver = std::function<SecurityManager*()>;
  void mountWs(AsyncWebServer* server, SecurityResolver resolveSecurity);

  // ==== SD log sink (toggleable) =========================================
  // When enabled, significant presence events (connect, disconnect, page
  // change) are appended to a rolling log file on the configured FS. Kept
  // explicitly off-by-default: a busy SPA can produce many rows per minute
  // and log writes go through async flash I/O. Admin toggles via UI; the
  // setter is idempotent and doesn't retroactively flush buffered events.
  //
  // The appends go through the flash write gate (power/WriteGate.h): a gate
  // pass on async_tcp, never a wait; when the gate says no, the line waits
  // in a bounded RAM queue (LOG_QUEUE_LINES / LOG_QUEUE_BYTES, the oldest
  // go when full, counted in logDropped()) and the FlashWriter job
  // "presence" appends it, one gate slot per batch of up to LOG_BATCH_BYTES.
  // While PowerGuard pauses writes the lines wait there.
  // setLogSink: at setup (it adds the FlashWriter job).
  void setLogSink(fs::FS* fs, const char* path = "/logs/presence.log",
                  size_t maxBytes = 64UL * 1024UL);
  void setLogEnabled(bool on);
  bool logEnabled() const { return _logEnabled; }
  size_t logPending() const;     // lines waiting for the write gate
  uint32_t logDropped() const;   // lines lost: queue full, or a failed write
  // Shutdown: what is queued, waiting for a gate slot boundMs at most.
  void flushLog(uint32_t boundMs);

  static constexpr size_t LOG_QUEUE_LINES = 64;
  static constexpr size_t LOG_QUEUE_BYTES = 4096;
  static constexpr size_t LOG_BATCH_BYTES = 4096;

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

  // Log file steps, each one bounded flash operation (the caller holds a
  // gate pass or slot and the writer flag).
  bool logOverCap() const;  // reads only
  void logRoll();           // removes the file
  bool logAppend(const char* data, size_t len);
  // The queued lines, a gate slot per batch (the FlashWriter job: boundMs 0).
  uint32_t drainLog(uint32_t boundMs);
  // One writer of the log file at a time (async_tcp, the FlashWriter task,
  // shutdown); a spinlock, never held across a write.
  bool claimLogWriter();
  void releaseLogWriter();
  void countLogLost(size_t lines);

  std::map<String, ClientPresence> _clients;  // keyed by clientKey

  AsyncWebSocket* _ws{nullptr};
  SecurityResolver _resolveSecurity;

  // SD/FS log sink state
  fs::FS*      _logFs{nullptr};
  String       _logPath;
  size_t       _logMaxBytes{0};
  bool         _logEnabled{false};

  // Lines the write gate put off (under _logMx, taken with a timeout).
  PresenceLogQueue<String> _logQueue{LOG_QUEUE_LINES, LOG_QUEUE_BYTES};
  SemaphoreHandle_t _logMx{nullptr};
  int8_t            _logJob{-1};
  // Under _logMux (a spinlock: plain fields, no atomics in a heap object).
  mutable portMUX_TYPE _logMux = portMUX_INITIALIZER_UNLOCKED;
  bool              _logWriting{false};
  uint32_t          _logLost{0};  // a failed write or a missed lock (the queue counts its own drops)
};

#endif  // PresenceService_h
