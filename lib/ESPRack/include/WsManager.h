#pragma once

// Single-socket, channel-multiplexed WebSocket manager for ESPAsyncWebServer.
//
// ONE AsyncWebSocket at "/ws" carries every feature's live state, multiplexed by
// a `ch` (channel = feature id) field in the JSON envelope. This collapses the
// former per-feature sockets into one connection per browser tab, which relieves
// the binding C3 constraint (the finite AsyncTCP PCB pool) and pays the WS auth
// handshake once.
//
// Wire envelope (flat JSON):
//   client->server: {"type":"sub","ch":"<id>"} / {"type":"unsub","ch":"<id>"}
//                   {"type":"p","ch":"<id>","origin_id":"...","p":{...}}   (state write)
//   server->client: {"type":"id","id":"ws:<cid>"}                          (once on connect)
//                   {"type":"p","ch":"<id>","origin_id":"...","p":{...}}    (push + snapshot)
// Resume = snapshot-on-(re)subscribe (server pushes current channel state when a
// client subscribes); NO replay buffer.
//
// Design notes:
// - ConfigManager-like wiring via WsManager::Binding<T> (the `path` argument is now
//   the CHANNEL name = feature id).
// - State access through StatefulService<T>::read / update.
// - Queues are FreeRTOS-free (portable) but correctly synchronised on ESP32.
// - One task pumps the queues: the loop task (App::loop -> processAllQueues).
//   WS events (async_tcp) and broadcasters only queue and wakeLoop(); the
//   loop's wait is bounded by waitMs() (ping / heartbeat deadlines).
// - No lock is held while sending (HGW-LIB-003): the roster lock covers map
//   work only, and every wait on it has WS_LOCK_TIMEOUT_MS and an error path.
//   A client being sent to is pinned (_pins): its disconnect, which frees it
//   on async_tcp, waits for that one send instead.

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#ifdef ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif

#include "LockWait.h"
#include "StatefulService.h"
#include "StateFace.h"
#include "WebAuth.h"

namespace ESPRack {
void wakeLoop();  // App.h: ends the loop task's wait now
}

#ifndef MWS_DEFAULT_JSON_CAP
#define MWS_DEFAULT_JSON_CAP 2048
#endif

// The single multiplexed WebSocket path. Distinct from /ws/presence (which stays
// its own always-on socket) so there is no path-prefix ambiguity. Override via -D.
#ifndef WS_MUX_PATH
#define WS_MUX_PATH "/ws/app"
#endif

// Device-wide cap on concurrent clients of the single multiplexed socket.
// AsyncWebSocket::cleanupClients(n) CLOSES THE OLDEST client whenever more
// than n are connected, so this is a hard limit on live browser tabs /
// devices. It used to be a literal 2 applied per feature socket; with one
// socket for everything that meant two clients per device. Each client
// holds one lwIP PCB (16-slot pool shared with HTTP and /ws/presence) plus
// its AsyncWebSocket queue, so the no-PSRAM C3 gets a conservative default.
// Override per consumer: -D ESPRACK_WS_MAX_CLIENTS=<n>.
//
// A client over the cap (or over ESPRACK_WS_MAX_PER_CHANNEL on the channel
// it subscribes to) closes one that nobody looks at, never a visible tab.
// The UI says whether its tab is shown ({type:"vis",hidden}); a tab hidden
// for 30 s closes its socket itself (usePageActive). So the one closed is:
//   0. first, at connect: the tab's own older socket (the page names its tab
//      in the URL, ?tab=), e.g. one a Wi-Fi drop left behind;
//   1. a client that missed its pong (dead; the pong timeout would take
//      it a minute later), the quietest first;
//   2. a hidden tab, the one hidden longest;
//   3. else a client that never said (an older UI build, a script), the one
//      that sent nothing (sub, unsub, a write) for the longest time; pongs
//      do not count, a browser answers them for a background tab too;
//   4. else, all the others being in view: the newcomer itself is refused.
// Closed with 1000 "evicted" / "busy", which Sockette does not retry: the
// UI shows Busy and tries again when the tab is shown or used, or after a
// minute. cleanupClients (one above the cap) only reaps closed clients.
#ifndef ESPRACK_WS_MAX_CLIENTS
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define ESPRACK_WS_MAX_CLIENTS 3
#else
#define ESPRACK_WS_MAX_CLIENTS 6
#endif
#endif
#ifndef ESPRACK_WS_MAX_PER_CHANNEL
#define ESPRACK_WS_MAX_PER_CHANNEL 4
#endif

// Per-client back-pressure threshold for outbound broadcasts. When a
// client's queue is at or above this depth, we skip the next frame for
// THAT client (fast clients keep getting broadcasts). Default is half of
// the library's hard limit WS_MAX_QUEUED_MESSAGES so we always have
// headroom before AsyncWebSocket's own overflow-close kicks in
// ("_queueMessage(): Too many messages queued: closing connection").
// Override via -D WS_BACKPRESSURE_THRESHOLD=<N> in platformio.ini.
#ifndef WS_BACKPRESSURE_THRESHOLD
#define WS_BACKPRESSURE_THRESHOLD (WS_MAX_QUEUED_MESSAGES / 2)
#endif

// App-level heartbeat: every WS_HEARTBEAT_MS each client gets {"type":"hb"},
// so the UI can tell a quiet channel from a dead socket (it reconnects after
// a few missed beats). A client whose queue stays at the back-pressure
// threshold for WS_STUCK_MS is stuck — the browser would keep showing Live
// with no data until a reload — and its connection is aborted, so the UI
// reconnects and subscribes again.
#ifndef WS_HEARTBEAT_MS
#define WS_HEARTBEAT_MS 5000
#endif
#ifndef WS_STUCK_MS
#define WS_STUCK_MS 20000
#endif

// Bounded waits (HGW-LIB-003). The roster lock guards map work only (never a
// send), so WS_LOCK_TIMEOUT_MS is reached only by a bug; a timeout drops that
// one piece of work (a frame, a stat, a sub) and is counted (lock_timeouts).
// A disconnect waits up to WS_SEND_WAIT_MS for a send to that client that is
// in flight on another task. The frame-doc lock covers a state read, so it
// waits for the state lock's timeout and a margin.
#ifndef WS_LOCK_TIMEOUT_MS
#define WS_LOCK_TIMEOUT_MS 500
#endif
#ifndef WS_SEND_WAIT_MS
#define WS_SEND_WAIT_MS 1000
#endif
#ifndef WS_FRAME_LOCK_TIMEOUT_MS
#define WS_FRAME_LOCK_TIMEOUT_MS (ESPRACK_STATE_LOCK_TIMEOUT_MS + 500)
#endif

// ---- Queue items ----
// Tx: cid==0 means "fan out to every client SUBSCRIBED to `channel`";
//     cid!=0 means "unicast to that client" (snapshot-on-subscribe).
struct WsTxItem {
  String channel;
  uint32_t cid;
  String payload;
  bool text;
};

// Rx: the channel lives INSIDE the payload (`ch`), so it isn't carried here.
struct WsRxItem {
  uint32_t cid;
  String payload;
  bool text;
};

class WsManager {
 public:
  explicit WsManager(AsyncWebServer* server, size_t queueCapacity = 10)
      : _server(server),
        _txQ(queueCapacity),
        _rxQ(queueCapacity),
        _pingIntervalSec(10),
        _pongTimeoutMs(30000),
        _lastPingTs(0),
        _hasFirstRtt(false),
        _alpha(0.30f),
        _avgRtt(0.0f) {
    // Guards the client roster (_clients incl. per-client subscription sets, and
    // _lastPong) against concurrent access from the two tasks that touch them:
    // async_tcp (WS events + form handlers/fillStats) and loopTask
    // (processAllQueues from App::loop). Held for map work only, never across
    // a send; recursive so a locked helper may call another one.
    _clientsMx = xSemaphoreCreateRecursiveMutex();
    // Guards the ONE shared outbound-build doc (_txDoc), reused for every
    // channel's state frame instead of a per-broadcast allocation.
    _txDocMx = xSemaphoreCreateRecursiveMutex();
    // One bit per pin slot: a disconnect waiting for a send (see _pins).
    _unpinned = xEventGroupCreate();
  }

  ~WsManager() {
    _channelList.clear();
    _channels.clear();
    _clients.clear();
    _lastPong.clear();
    if (_ws) { delete _ws; _ws = nullptr; }
    if (_clientsMx) vSemaphoreDelete(_clientsMx);
    if (_txDocMx) vSemaphoreDelete(_txDocMx);
    if (_unpinned) vEventGroupDelete(_unpinned);
  }

  // =======================================================================
  // ConfigManager-style helper: Binding<TState>  (path == channel name)
  // =======================================================================
  template <typename TState>
  class Binding {
   public:
    Binding() = default;

    Binding(WsManager* manager,
            const char* path,
            size_t jsonCapacity,
            StatefulService<TState>* service,
            JsonStateReader<TState> reader,
            JsonStateUpdater<TState> updater,
            bool autoBroadcast = true,
            WebAuthLevel level = WebAuthLevel::Authenticated)
        : _mgr(manager),
          _path(path ? String(path) : String()),
          _service(service),
          _jsonCapacity(jsonCapacity ? jsonCapacity : (size_t)MWS_DEFAULT_JSON_CAP),
          _reader(reader),
          _updater(updater),
          _autoBroadcast(autoBroadcast),
          _level(level) {
      attach();
    }

    void attach() {
      if (_attached) return;
      if (!_mgr || !_service || _path.length() == 0) return;

      _mgr->template addEndpoint<TState>(_path, _service, _reader, _updater, _jsonCapacity, _level);

      // Auto-broadcast on any service update (REST/MQTT/WS); origin lets the
      // frontend filter its own echoes.
      if (_autoBroadcast && !_handlerId) {
        _handlerId = addHandlerCompat(
            _service,
            [this](const String& origin) {
              if (_mgr) _mgr->broadcastCurrentState(_path, origin);
            },
            false,
            0);
      }

      _attached = true;
    }

    void broadcastCurrentState(const String& origin = "") {
      if (_mgr && _path.length()) _mgr->broadcastCurrentState(_path, origin);
    }

    const String& path() const { return _path; }

   private:
    WsManager* _mgr{nullptr};
    String _path;

    StatefulService<TState>* _service{nullptr};
    size_t _jsonCapacity{MWS_DEFAULT_JSON_CAP};
    JsonStateReader<TState> _reader;
    JsonStateUpdater<TState> _updater;
    bool _autoBroadcast{true};
    WebAuthLevel _level{WebAuthLevel::Authenticated};

    bool _attached{false};
    update_handler_id_t _handlerId{0};

    template <typename TSvc, typename TFn>
    static auto addHandlerCompat(TSvc* svc, TFn fn, bool b, int)
        -> decltype(svc->addUpdateHandler(fn, b), update_handler_id_t()) {
      return svc->addUpdateHandler(fn, b);
    }

    template <typename TSvc, typename TFn>
    static auto addHandlerCompat(TSvc* svc, TFn fn, bool /*b*/, long)
        -> decltype(svc->addUpdateHandler(fn), update_handler_id_t()) {
      return svc->addUpdateHandler(fn);
    }

    template <typename TSvc, typename TFn>
    static update_handler_id_t addHandlerCompat(TSvc*, TFn, bool, ...) {
      return 0;
    }
  };

  // =======================================================================
  // Channel registration
  // =======================================================================

  // C3 (api-v1): upgrade-time auth gate applied ONCE to the single /ws socket.
  // The ?access_token= on the WS URL is validated at the WebSocket upgrade;
  // a raw connect without a valid token is rejected before any frame flows.
  // When no filter is set (FT_SECURITY=0 / Null manager) behaviour stays open.
  void setAuthFilter(ArRequestFilterFunction filter) {
    _authFilter = filter;
    if (_ws) _ws->setFilter(_authFilter);
  }

  // Per-channel authorization (LIB-FIX-003). The upgrade filter only proves
  // "authenticated"; each connection's identity is resolved once, in the
  // WebSocket handshake handler (the only point where the upgrade request is
  // still alive — AsyncWebSocketClient deletes it before WS_EVT_CONNECT), and
  // handed to WS_EVT_CONNECT by TCP connection. Into an access level + write right:
  //   sub on channel X  -> client level >= X's feature auth level
  //   "p" write on X    -> the above AND operator role (not a "reader" token)
  // Without a manager (no SecurityModule / FT_SECURITY=0) every client is
  // treated as admin+operator, matching the open-device behaviour elsewhere.
  // Identity is fixed for the connection's lifetime: a revoked token keeps
  // its socket until it reconnects (ping eviction / page reload).
  void setSecurityManager(SecurityManager* sm) { _sm = sm; }

  // Per-key write levels for a channel (LIB-FIX-011): a "p" frame needs
  // fn(payload) instead of the channel's level. Set by WebFeatureEntry.
  void setChannelWriteLevel(const String& channel,
                            std::function<WebAuthLevel(JsonObjectConst)> fn) {
    auto it = _channels.find(channel);
    if (it != _channels.end() && it->second) it->second->writeLevel = std::move(fn);
  }

  // `path` is the CHANNEL name (feature id). All channels share one socket.
  template <typename TState>
  void* addEndpoint(const String& path,
                    StatefulService<TState>* svc,
                    JsonStateReader<TState> reader,
                    JsonStateUpdater<TState> updater,
                    size_t jsonCapacity = MWS_DEFAULT_JSON_CAP,
                    WebAuthLevel level = WebAuthLevel::Authenticated) {
    if (!_server || !svc || path.length() == 0) {
      return nullptr;
    }

    ensureSocket();

    auto existing = _channels.find(path);
    if (existing != _channels.end()) {
      return existing->second;
    }

    std::unique_ptr<Channel<TState>> ch(new Channel<TState>(path, svc, reader, updater, jsonCapacity));
    ch->level = level;
    ChannelBase* chPtr = ch.get();

    _channels[path] = chPtr;
    _channelList.push_back(std::move(ch));

    // The shared outbound-build doc is sized to the LARGEST channel only — not
    // the sum. All channels reuse that one buffer (see buildStateFrame).
    if (jsonCapacity > _maxWsCap) _maxWsCap = jsonCapacity;
    return chPtr;
  }

  // =======================================================================
  // Sending API  (channel == feature id)
  // =======================================================================
  void broadcast(const String& channel, const String& payload, bool text = true) {
    enqueueTx(channel, 0, payload, text);
  }

  void sendTo(const String& channel, uint32_t cid, const String& payload, bool text = true) {
    enqueueTx(channel, cid, payload, text);
  }

  // Subscriber introspection — used by services to gate expensive work
  // (periodic ticks, per-message echoes) before building any payload.
  bool hasClients(const String& channel) const { return subscriberCount(channel) > 0; }
  size_t clientCount(const String& channel) const { return subscriberCount(channel); }

  // For Resources: clients now (not counting ones being closed), the most
  // since boot, and how many were closed by the caps / as stuck.
  size_t totalClients() const {
    ClientsLock g(_clientsMx);
    if (!g) {
      lockTimeout("count");
      return 0;
    }
    return liveCount();
  }
  size_t peakClients() const { return _peakClients; }
  uint32_t evictions() const { return _evictions; }
  uint32_t refusals() const { return _refusals; }
  uint32_t stuckEvictions() const { return _stuckEvictions; }

  // Serialise the registry for the admin Clients/Endpoints tabs: every channel
  // with the clients currently SUBSCRIBED to it (id, ip, age, seconds since last
  // rx/tx). Shape kept compatible with the old per-endpoint layout
  // (endpoints[].clients[]) so WebEndpointsService/WsDiag need no change.
  void fillStats(JsonObject& root) const {
    ClientsLock g(_clientsMx);
    if (!g) {
      lockTimeout("stats");
      root["busy"] = true;
      root["lock_timeouts"] = _lockTimeouts;
      return;
    }
    const unsigned long now = millis();
    auto sinceSec = [now](uint32_t ts) -> uint32_t {
      if (ts == 0) return 0;
      return (uint32_t)((now - ts) / 1000UL);
    };

    JsonArray eps = root.createNestedArray("endpoints");
    uint32_t totalBroadcastsDropped = 0;
    uint32_t totalOverflow = 0;
    for (auto& chPair : _channels) {
      ChannelBase* ch = chPair.second;
      if (!ch) continue;
      JsonObject j = eps.createNestedObject();
      j["path"] = ch->name;
      totalBroadcastsDropped += ch->droppedBroadcasts;
      j["dropped_broadcasts"] = ch->droppedBroadcasts;
      totalOverflow += ch->overflowFrames;
      j["overflow_frames"] = ch->overflowFrames;
      JsonArray cl = j.createNestedArray("clients");
      size_t cnt = 0;
      for (auto& kv : _clients) {
        if (kv.second.subs.find(ch->name) == kv.second.subs.end()) continue;
        cnt++;
        JsonObject c = cl.createNestedObject();
        c["id"]        = kv.second.id;
        c["ip"]        = kv.second.remoteIp;
        c["age_s"]     = sinceSec(kv.second.connectedAtMs);
        c["last_rx_s"] = sinceSec(kv.second.lastRxMs);
        c["last_tx_s"] = kv.second.lastTxMs == 0 ? (uint32_t)0 : sinceSec(kv.second.lastTxMs);
        c["drops"]     = kv.second.droppedFrames;
        c["slow_s"]    = sinceSec(kv.second.slowSinceMs);
        c["view"]      = kv.second.hidden < 0 ? "?" : kv.second.hidden ? "hidden" : "shown";
      }
      j["count"] = cnt;
    }

    uint32_t totalDropped = 0;
    for (auto& kv : _clients) totalDropped += kv.second.droppedFrames;

    root["total_clients"]  = _clients.size();   // distinct connections on the one socket
    root["max_clients"]    = (uint32_t)ESPRACK_WS_MAX_CLIENTS;
    root["max_per_channel"] = (uint32_t)ESPRACK_WS_MAX_PER_CHANNEL;
    root["peak_clients"]   = (uint32_t)_peakClients;
    root["evictions"]      = _evictions;
    root["refusals"]       = _refusals;
    root["avg_rtt_ms"]     = _avgRtt;
    root["ping_interval_s"] = _pingIntervalSec;
    root["pong_timeout_ms"] = _pongTimeoutMs;
    root["total_dropped"]  = totalDropped;
    root["total_broadcasts_dropped"] = totalBroadcastsDropped;
    root["total_overflow_frames"]    = totalOverflow;
    root["backpressure_threshold"]   = (uint32_t)WS_BACKPRESSURE_THRESHOLD;
    root["stuck_evictions"]          = _stuckEvictions;
    root["backpressure_max"]         = (uint32_t)WS_MAX_QUEUED_MESSAGES;
    root["lock_timeouts"]            = _lockTimeouts;
  }

  // Push current channel state to ALL its subscribers.
  void broadcastCurrentState(const String& channel, const String& origin = "") {
    ChannelBase* base = findChannel(channel);
    if (!base || !_ws) return;
    // Belt-and-suspenders: skip the serialise when nobody is subscribed.
    // Services should also gate earlier via hasClients(channel).
    if (subscriberCount(channel) == 0) return;
    String out;
    if (!buildStateFrame(base, origin, out, ESPRack::StateFace::WsPush)) return;
    broadcast(channel, out, true);
  }

  // Processing queues: the loop task only (App::loop). Events and
  // broadcasters queue and wake it; waitMs() says when it is due anyway.
  void processAllQueues() {
    // Reap closed clients. The cap itself is WS_EVT_CONNECT's: one above it,
    // so this can't close a second client while that one picks its victim.
    if (_ws) _ws->cleanupClients(ESPRACK_WS_MAX_CLIENTS + 1);
    purgeGone();
    processRx();
    processTx();
    // Keepalive, on the same task as the queue pump. Gated by interval.
    if (_pingIntervalSec &&
        (millis() - _lastPingTs >= (unsigned long)_pingIntervalSec * 1000UL)) {
      pingAll();          // updates _lastPingTs
      checkInactive();
    }
    if (millis() - _lastHeartbeatMs >= WS_HEARTBEAT_MS) {
      _lastHeartbeatMs = millis();
      heartbeat();
    }
  }

  // How long the loop may wait from nowMs before processAllQueues() must run
  // again: 0 with frames queued, else the nearer of the ping and heartbeat
  // deadlines. Queueing a frame wakes the loop (ESPRack::wakeLoop).
  uint32_t waitMs(uint32_t nowMs) const {
    if (!_rxQ.empty() || !_txQ.empty()) return 0;
    uint32_t w = ESPRack::msUntilDue(_lastHeartbeatMs, WS_HEARTBEAT_MS, nowMs);
    if (_pingIntervalSec) {
      const uint32_t p = ESPRack::msUntilDue((uint32_t)_lastPingTs, _pingIntervalSec * 1000u, nowMs);
      if (p < w) w = p;
    }
    return w;
  }

  // Ping/Pong keepalive — cadence only; the pass runs inline in processAllQueues.
  void beginPingPong(unsigned pingIntervalSec = 10, unsigned long pongTimeoutMs = 30000) {
    _pingIntervalSec = pingIntervalSec;
    _pongTimeoutMs = pongTimeoutMs;
  }

  float averageRTT() const { return _avgRtt; }

 private:
  // --------- Portable pointer ring queue ---------
  template <typename T>
  class PtrRingQueue {
   public:
    explicit PtrRingQueue(size_t cap) : _cap(cap), _buf(new T*[cap]()) {}

    ~PtrRingQueue() {
      if (_buf) {
        for (size_t i = 0; i < _cap; ++i) {
          delete _buf[i];
          _buf[i] = nullptr;
        }
        delete[] _buf;
        _buf = nullptr;
      }
    }

    bool push(T* item) {
      if (!item || _cap == 0) return false;
      lock();
      if (_size >= _cap) {
        unlock();
        return false;
      }
      _buf[_tail] = item;
      _tail = (_tail + 1) % _cap;
      ++_size;
      unlock();
      return true;
    }

    bool empty() const {
      lock();
      const bool e = _size == 0;
      unlock();
      return e;
    }

    bool pop(T*& out) {
      lock();
      if (_size == 0) {
        unlock();
        return false;
      }
      out = _buf[_head];
      _buf[_head] = nullptr;
      _head = (_head + 1) % _cap;
      --_size;
      unlock();
      return true;
    }

   private:
    size_t _cap{0};
    T** _buf{nullptr};
    size_t _head{0};
    size_t _tail{0};
    size_t _size{0};

#ifdef ESP32
    mutable portMUX_TYPE _mx = portMUX_INITIALIZER_UNLOCKED;
    void lock() const { portENTER_CRITICAL(&_mx); }
    void unlock() const { portEXIT_CRITICAL(&_mx); }
#else
    void lock() const { noInterrupts(); }
    void unlock() const { interrupts(); }
#endif
  };

  // Per-client bookkeeping for diagnostics, phantom-eviction, and the set of
  // channels this client is subscribed to. Populated by handleWsEvent /
  // processRx; read by fillStats(). Guarded by _clientsMx.
  struct WsClientInfo {
    uint32_t id{0};
    String   remoteIp;
    uint32_t connectedAtMs{0};
    uint32_t lastRxMs{0};
    uint32_t lastAppRxMs{0};     // last frame from the page itself (not a pong)
    uint32_t lastTxMs{0};
    uint32_t droppedFrames{0};   // frames skipped due to per-client back-pressure
    uint32_t slowSinceMs{0};     // since when this client's queue is full (0 = it moves)
    std::set<String> subs;       // channels this client subscribed to
    // Resolved once at connect (see setSecurityManager).
    WebAuthLevel level{WebAuthLevel::Admin};
    bool canWrite{true};
    bool evicting{false};        // closed by a cap, waiting for its disconnect
    int8_t hidden{-1};           // the page says: 1 hidden, 0 shown, -1 never said
    uint32_t hiddenSinceMs{0};
    uint32_t tab{0};             // the page's tab (see tabHash), 0 unknown
  };

  // --------- Channel type-erasure ---------
  struct ChannelBase {
    String name;                              // channel id (== feature id)
    WebAuthLevel level{WebAuthLevel::Authenticated};  // the feature's auth level
    // Optional per-key write level (LIB-FIX-011); unset = `level`.
    std::function<WebAuthLevel(JsonObjectConst)> writeLevel;
    size_t jsonCapacity{MWS_DEFAULT_JSON_CAP};
    uint32_t droppedBroadcasts{0};            // skipped because no subscriber was ready
    uint32_t overflowFrames{0};               // dropped: the state didn't fit the frame doc

    virtual ~ChannelBase() {}
    virtual bool readInto(JsonObject& out) = 0;  // false: state lock busy
    virtual StateUpdateResult apply(JsonObject& in, const String& origin) = 0;
  };

  // Prefer svc->update(data, updater, origin), fallback svc->update(data, updater),
  // fallback svc->updateWithoutPropagation(data, updater)
  template <typename TSvc, typename TUpdater>
  static auto callUpdate(TSvc* svc, JsonObject& data, TUpdater updater, const String& origin, int)
      -> decltype(svc->update(data, updater, origin), StateUpdateResult()) {
    return svc->update(data, updater, origin);
  }

  template <typename TSvc, typename TUpdater>
  static auto callUpdate(TSvc* svc, JsonObject& data, TUpdater updater, const String& /*origin*/, long)
      -> decltype(svc->update(data, updater), StateUpdateResult()) {
    return svc->update(data, updater);
  }

  template <typename TSvc, typename TUpdater>
  static StateUpdateResult callUpdate(TSvc* svc, JsonObject& data, TUpdater updater, const String& /*origin*/, ...) {
    svc->updateWithoutPropagation(data, updater);
    return StateUpdateResult::CHANGED;
  }

  template <typename TState>
  struct Channel : public ChannelBase {
    Channel(const String& n,
            StatefulService<TState>* s,
            JsonStateReader<TState> r,
            JsonStateUpdater<TState> u,
            size_t cap)
        : svc(s), reader(r), updater(u) {
      name = n;
      jsonCapacity = cap ? cap : (size_t)MWS_DEFAULT_JSON_CAP;
    }

    StatefulService<TState>* svc{nullptr};
    JsonStateReader<TState> reader;
    JsonStateUpdater<TState> updater;

    bool readInto(JsonObject& out) override {
      if (!svc) return false;
      return svc->read(out, reader);
    }

    StateUpdateResult apply(JsonObject& in, const String& origin) override {
      if (!svc) return StateUpdateResult::UNCHANGED;
      return callUpdate(svc, in, updater, origin, 0);
    }
  };

  // --------- internals ---------
  AsyncWebServer* _server{nullptr};
  AsyncWebSocket* _ws{nullptr};                 // THE single multiplexed socket
  ArRequestFilterFunction _authFilter{nullptr};
  SecurityManager* _sm{nullptr};  // per-connection identity (LIB-FIX-003)

  // Identities resolved at the handshake, waiting for their WS_EVT_CONNECT.
  // Keyed by the TCP connection (AsyncClient*), which the WebSocket client
  // keeps. A few slots suffice: the connect follows its handshake within one
  // request cycle; stale entries (failed upgrades) age out. Guarded by _clientsMx.
  struct PendingIdentity {
    const void* tcp = nullptr;
    WebAuthLevel level = WebAuthLevel::Public;
    bool canWrite = false;
    uint32_t tab = 0;  // the page's ?tab= (hashed), 0 none
    uint32_t atMs = 0;
  };
  static constexpr size_t kPendingIdentities = 6;
  static constexpr uint32_t kPendingIdentityTtlMs = 10000;
  PendingIdentity _pendingIds[kPendingIdentities];

  // FNV-1a of the page's tab id; 0 is kept for "none".
  static uint32_t tabHash(const String& s) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < s.length(); ++i) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h ? h : 1;
  }

  // Handshake handler (async_tcp task): the request is valid here.
  bool stashIdentity(AsyncWebServerRequest* r) {
    if (!r) return true;
    PendingIdentity p;
    p.tcp = r->client();
    if (const AsyncWebParameter* t = r->getParam("tab")) {
      if (t->value().length()) p.tab = tabHash(t->value());
    }
    if (_sm) {
      Authentication au = _sm->authenticateRequest(r);  // may be slow: outside the lock
      p.level = !au.authenticated ? WebAuthLevel::Public
              : (au.user && au.user->admin) ? WebAuthLevel::Admin
              : WebAuthLevel::Authenticated;
      p.canWrite = AuthenticationPredicates::IS_OPERATOR(au);
    } else if (!p.tab) {
      return true;  // nothing to carry to the connect
    }
    p.atMs = millis();
    {
      ClientsLock g(_clientsMx);
      if (!g) {
        // The connect finds no identity: Public, no write (fail closed).
        lockTimeout("identity");
        return true;
      }
      // One entry per connection: drop this connection's older entry and
      // anything expired, then take a free slot (else the oldest).
      for (auto& e : _pendingIds) {
        if (e.tcp && (e.tcp == p.tcp || p.atMs - e.atMs > kPendingIdentityTtlMs)) e = PendingIdentity{};
      }
      size_t slot = 0;
      for (size_t i = 0; i < kPendingIdentities; ++i) {
        if (!_pendingIds[i].tcp) { slot = i; break; }
        if (_pendingIds[i].atMs < _pendingIds[slot].atMs) slot = i;
      }
      _pendingIds[slot] = p;
    }
    // A handshake that never becomes a WebSocket (400, abort before the 101 is
    // ACKed) ends with the request's disconnect: drop its entry, so a later
    // connection whose AsyncClient reuses this address can't inherit it. A
    // successful upgrade rebinds the connection's callbacks, so this never
    // fires for live sockets (and deleting the request doesn't fire it).
    const void* tcp = p.tcp;
    r->onDisconnect([this, tcp]() { dropIdentity(tcp); });
    return true;  // admission is the upgrade filter's job
  }

  void dropIdentity(const void* tcp) {
    ClientsLock g(_clientsMx);
    if (!g) {
      lockTimeout("identity");
      return;  // the entry ages out (kPendingIdentityTtlMs)
    }
    for (auto& e : _pendingIds) {
      if (e.tcp == tcp) e = PendingIdentity{};
    }
  }

  // Caller holds _clientsMx. With a security manager, an unknown connection
  // is Public, no write (fail closed); without one the caller's open
  // defaults stay. `tab`: the page's tab, 0 if none.
  void takeIdentity(const void* tcp, WebAuthLevel& level, bool& canWrite, uint32_t& tab) {
    if (_sm) {
      level = WebAuthLevel::Public;
      canWrite = false;
    }
    tab = 0;
    const uint32_t now = millis();
    for (auto& p : _pendingIds) {
      if (!p.tcp || p.tcp != tcp) continue;
      if (now - p.atMs <= kPendingIdentityTtlMs) {
        if (_sm) {
          level = p.level;
          canWrite = p.canWrite;
        }
        tab = p.tab;
      }
      p = PendingIdentity{};
      break;  // at most one entry per connection (stashIdentity dedupes)
    }
  }

  static bool levelAtLeast(WebAuthLevel have, WebAuthLevel need) {
    return static_cast<uint8_t>(have) >= static_cast<uint8_t>(need);
  }

  // Copy of a client's access rights, taken under the roster lock. Unknown
  // client (already gone) -> no rights.
  bool clientRights(uint32_t cid, WebAuthLevel& level, bool& canWrite) const {
    ClientsLock g(_clientsMx);
    if (!g) {
      lockTimeout("rights");
      return false;
    }
    auto cit = _clients.find(cid);
    if (cit == _clients.end()) return false;
    level = cit->second.level;
    canWrite = cit->second.canWrite;
    return true;
  }

  std::vector<std::unique_ptr<ChannelBase>> _channelList;
  std::map<String, ChannelBase*> _channels;

  // Single client roster (one socket). Guarded by _clientsMx.
  std::map<uint32_t, WsClientInfo> _clients;

  PtrRingQueue<WsTxItem> _txQ;
  PtrRingQueue<WsRxItem> _rxQ;

  // ping/pong — driven inline from processAllQueues().
  unsigned _pingIntervalSec;
  unsigned long _pongTimeoutMs;
  unsigned long _lastPingTs;
  std::map<uint32_t, unsigned long> _lastPong;
  uint32_t _lastHeartbeatMs{0};
  uint32_t _stuckEvictions{0};
  uint32_t _evictions{0};
  uint32_t _refusals{0};
  size_t _peakClients{0};

  SemaphoreHandle_t _clientsMx = nullptr;
  // Recursive-mutex guard with a timeout: test it (`if (!g)`) and take the
  // error path when the lock stayed busy.
  struct ClientsLock {
    SemaphoreHandle_t h;
    bool ok;
    explicit ClientsLock(SemaphoreHandle_t s, uint32_t timeoutMs = WS_LOCK_TIMEOUT_MS)
        : h(s), ok(!s || xSemaphoreTakeRecursive(s, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {}
    ~ClientsLock() { if (h && ok) xSemaphoreGiveRecursive(h); }
    explicit operator bool() const { return ok; }
    ClientsLock(const ClientsLock&) = delete;
    ClientsLock& operator=(const ClientsLock&) = delete;
  };
  mutable uint32_t _lockTimeouts{0};  // any WsManager lock (stats: lock_timeouts)

  // Lock timed out in `where`: count; log the 1st, 2nd, 4th...
  void lockTimeout(const char* where) const {
    const uint32_t n = ++_lockTimeouts;
    if (ESPRack::logThisFail(n)) log_w("[ws] lock busy (%s), %u so far", where, (unsigned)n);
  }

  // Liveness of the library's clients. An AsyncWebSocketClient is freed by
  // async_tcp (its destructor fires WS_EVT_DISCONNECT) while the loop task
  // may be sending to it. A sender pins the client (pin/unpin, any task);
  // the disconnect closes its entry and waits for those pins (releaseClient).
  // The pointer comes from WS_EVT_CONNECT, so senders never walk the
  // library's unlocked client list. Spinlock: a few loads/stores per call.
  static constexpr size_t kPinSlots =
      (2 * ESPRACK_WS_MAX_CLIENTS + 2) < 24 ? (2 * ESPRACK_WS_MAX_CLIENTS + 2) : 24;
  ESPRack::PinTable<AsyncWebSocketClient, kPinSlots> _pins;
  mutable portMUX_TYPE _pinMx = portMUX_INITIALIZER_UNLOCKED;
  EventGroupHandle_t _unpinned = nullptr;

  AsyncWebSocketClient* pin(uint32_t cid) {
    const void* me = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&_pinMx);
    AsyncWebSocketClient* c = _pins.pin(cid, me);
    portEXIT_CRITICAL(&_pinMx);
    return c;
  }

  void unpin(uint32_t cid) {
    const void* me = xTaskGetCurrentTaskHandle();
    int slot = -1;
    portENTER_CRITICAL(&_pinMx);
    const bool wake = _pins.unpin(cid, me, slot);
    portEXIT_CRITICAL(&_pinMx);
    if (wake && _unpinned) xEventGroupSetBits(_unpinned, (EventBits_t)1 << slot);
  }

  // WS_EVT_DISCONNECT, inside the client's destructor: no new pins, then wait
  // (WS_SEND_WAIT_MS) for a send in flight on another task. Timeout: logged,
  // the destructor goes on.
  void releaseClient(uint32_t cid) {
    const void* me = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&_pinMx);
    const int slot = _pins.close(cid, me);
    portEXIT_CRITICAL(&_pinMx);
    if (slot < 0) return;
    const EventBits_t bit = (EventBits_t)1 << slot;
    const TickType_t t0 = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(WS_SEND_WAIT_MS);
    for (;;) {
      portENTER_CRITICAL(&_pinMx);
      const bool busy = _pins.waiting(slot);
      if (!busy) _pins.done(slot);
      portEXIT_CRITICAL(&_pinMx);
      if (!busy) break;
      const TickType_t spent = xTaskGetTickCount() - t0;
      if (!_unpinned || spent >= limit) {
        log_e("[ws] client %u freed while a send to it is in flight", (unsigned)cid);
        portENTER_CRITICAL(&_pinMx);
        _pins.done(slot);
        portEXIT_CRITICAL(&_pinMx);
        break;
      }
      xEventGroupWaitBits(_unpinned, bit, pdTRUE, pdTRUE, limit - spent);
    }
    if (_unpinned) xEventGroupClearBits(_unpinned, bit);
  }

  // Whether the library client is connected (its entry open: not freed).
  bool clientConnected(uint32_t cid) const {
    portENTER_CRITICAL(&_pinMx);
    const AsyncWebSocketClient* c = _pins.peek(cid);
    const bool on = c && c->status() == WS_CONNECTED;
    portEXIT_CRITICAL(&_pinMx);
    return on;
  }

  // Roster entries whose client is gone without its disconnect getting the
  // roster lock (a timeout there): dropped on the next pump.
  void purgeGone() {
    ClientsLock g(_clientsMx);
    if (!g) {
      lockTimeout("purge");
      return;
    }
    for (auto it = _clients.begin(); it != _clients.end();) {
      portENTER_CRITICAL(&_pinMx);
      const bool known = _pins.has(it->first);
      portEXIT_CRITICAL(&_pinMx);
      if (known) {
        ++it;
        continue;
      }
      _lastPong.erase(it->first);
      it = _clients.erase(it);
    }
  }

  // ONE shared outbound-build doc, reused for every channel's state frame
  // (sized to the LARGEST channel only — _maxWsCap, not the sum). Eliminates
  // the per-broadcast alloc/free churn (notably LightState's ~10 Hz chart) and
  // bounds WS build memory to a single max-sized buffer. Guarded by _txDocMx
  // since broadcasts originate from several tasks (loop/async_tcp/mqtt/...).
  size_t _maxWsCap{MWS_DEFAULT_JSON_CAP};
  std::unique_ptr<DynamicJsonDocument> _txDoc;
  SemaphoreHandle_t _txDocMx = nullptr;

  // RTT
  bool _hasFirstRtt;
  float _alpha;
  float _avgRtt;

  void ensureSocket() {
    if (_ws || !_server) return;
    _ws = new AsyncWebSocket(WS_MUX_PATH);
    _ws->onEvent([this](AsyncWebSocket* /*s*/, AsyncWebSocketClient* c, AwsEventType t, void* a,
                        uint8_t* d, size_t l) {
      if (c) this->handleWsEvent(c, t, a, d, l);
    });
    _ws->handleHandshake([this](AsyncWebServerRequest* r) { return this->stashIdentity(r); });
    if (_authFilter) _ws->setFilter(_authFilter);
    _server->addHandler(_ws);
  }

  ChannelBase* findChannel(const String& name) {
    auto it = _channels.find(name);
    return (it == _channels.end()) ? nullptr : it->second;
  }

  // Under _clientsMx. Whether a roster entry still counts: not closed by a
  // cap, and still connected for the library (client() is null once it is
  // closing, e.g. the page closed it; the same rule as its count()).
  bool isLive(const std::pair<const uint32_t, WsClientInfo>& kv) const {
    return !kv.second.evicting && clientConnected(kv.first);
  }

  // Under _clientsMx. Live clients (on `channel` if given).
  size_t liveCount(const String* channel = nullptr) const {
    size_t n = 0;
    for (auto& kv : _clients) {
      if (!isLive(kv)) continue;
      if (channel && kv.second.subs.find(*channel) == kv.second.subs.end()) continue;
      n++;
    }
    return n;
  }

  // Under _clientsMx. The client to close for `keep` (see the cap note at
  // the top; on `channel` if given), marked evicting; `keep` itself when no
  // other one may go.
  uint32_t pickEviction(const String* channel, uint32_t keep) {
    const uint32_t now = millis();
    // Silent past one ping and a margin: it missed its pong.
    const uint32_t deadMs = _pingIntervalSec ? _pingIntervalSec * 1000u + 5000u : 0;
    WsClientInfo* dead = nullptr;
    WsClientInfo* hidden = nullptr;
    WsClientInfo* unknown = nullptr;
    for (auto& kv : _clients) {
      WsClientInfo& ci = kv.second;
      if (kv.first == keep || !isLive(kv)) continue;
      if (channel && ci.subs.find(*channel) == ci.subs.end()) continue;
      if (deadMs && now - ci.lastRxMs > deadMs) {
        if (!dead || now - ci.lastRxMs > now - dead->lastRxMs) dead = &ci;
      } else if (ci.hidden > 0) {
        if (!hidden || now - ci.hiddenSinceMs > now - hidden->hiddenSinceMs) hidden = &ci;
      } else if (ci.hidden < 0) {
        if (!unknown || now - ci.lastAppRxMs > now - unknown->lastAppRxMs) unknown = &ci;
      }
    }
    WsClientInfo* victim = dead ? dead : hidden ? hidden : unknown;
    if (!victim) {
      auto self = _clients.find(keep);
      if (self != _clients.end()) self->second.evicting = true;
      return keep;
    }
    victim->evicting = true;
    return victim->id;
  }

  // Closes a client picked by pickEviction, with no lock held: the client is
  // pinned, so its disconnect waits for close() to return.
  void evict(uint32_t cid, bool refused, const char* why) {
    if (!cid || !_ws) return;
    if (refused) ++_refusals;
    else ++_evictions;
    log_w("[ws] client %u %s: %s", (unsigned)cid, refused ? "refused" : "closed", why);
    if (AsyncWebSocketClient* c = pin(cid)) {
      c->close(1000, refused ? "busy" : "evicted");
      unpin(cid);
    }
  }

  // Lock busy: 1, so a service still builds its push (processTx drops it
  // when nobody is subscribed) instead of losing it.
  size_t subscriberCount(const String& channel) const {
    ClientsLock g(_clientsMx);
    if (!g) {
      lockTimeout("subs");
      return 1;
    }
    size_t n = 0;
    for (auto& kv : _clients) {
      if (kv.second.subs.find(channel) != kv.second.subs.end()) n++;
    }
    return n;
  }

  // Build {"type":"p","ch":<channel>,"origin_id":<origin>,"p":{...state...}}
  // into the ONE shared, reused doc (sized to the largest channel, _maxWsCap).
  // The lock serialises builds across the several tasks that can broadcast
  // (loop/async_tcp/mqtt/...); ws sends happen later in processTx, NOT here, so
  // the lock is never held across a socket write. ClientsLock is just a generic
  // recursive-mutex RAII guard, reused here for _txDocMx.
  bool buildStateFrame(ChannelBase* base, const String& origin, String& out, ESPRack::StateFace face) {
    if (!base) return false;
    ClientsLock g(_txDocMx, WS_FRAME_LOCK_TIMEOUT_MS);
    if (!g) {
      lockTimeout("frame");
      return false;
    }
    // readers can tell this frame (fixed buffer) from a REST read.
    ESPRack::StateFaceScope faceScope(face);
    const size_t want = _maxWsCap + 256;
    if (!_txDoc || _txDoc->capacity() < want) {
      _txDoc.reset(new DynamicJsonDocument(want));   // one-time grow (channels register at boot)
    }
    if (!_txDoc || _txDoc->capacity() == 0) return false;
    _txDoc->clear();
    JsonObject root = _txDoc->to<JsonObject>();
    root["type"] = "p";
    root["ch"] = base->name;
    root["origin_id"] = origin;
    JsonObject p = root.createNestedObject("p");
    // State lock busy (StatefulService timeout): no frame rather than an
    // empty state; the next push or subscribe sends it.
    if (!base->readInto(p)) return false;
    // A reader that outgrew the doc leaves a silently truncated tree: drop
    // the frame instead of sending half a state (as DataApi).
    if (_txDoc->overflowed()) {
      const uint32_t n = ++base->overflowFrames;
      if ((n & (n - 1)) == 0) {  // 1st, 2nd, 4th, 8th...: no log flood at 10 Hz
        log_w("[ws] frame overflow on '%s' (doc %u B), dropped (%u so far)", base->name.c_str(),
              (unsigned)_txDoc->capacity(), (unsigned)n);
      }
      return false;
    }
    serializeJson(*_txDoc, out);
    return true;
  }

  // Snapshot-on-subscribe: push the channel's current state to ONE client.
  void pushSnapshot(const String& channel, uint32_t cid) {
    ChannelBase* base = findChannel(channel);
    if (!base) return;
    String out;
    if (!buildStateFrame(base, "ws_sub", out, ESPRack::StateFace::WsSnapshot)) return;
    sendTo(channel, cid, out, true);
  }

  // Both queues are pumped by the loop task: wake it.
  void enqueueTx(const String& channel, uint32_t cid, const String& payload, bool text) {
    auto* it = new WsTxItem{channel, cid, payload, text};
    if (!_txQ.push(it)) delete it;
    else ESPRack::wakeLoop();
  }

  void enqueueRx(uint32_t cid, const String& payload, bool text) {
    auto* it = new WsRxItem{cid, payload, text};
    if (!_rxQ.push(it)) delete it;
    else ESPRack::wakeLoop();
  }

  // Per frame: the targets are copied under the roster lock; each send is
  // outside it, to a pinned client; the result is booked under the lock.
  // Lock busy: that frame is dropped (counted).
  void processTx() {
    WsTxItem* it = nullptr;
    if (!_ws) {
      while (_txQ.pop(it)) delete it;
      return;
    }
    std::vector<uint32_t> to;
    while (_txQ.pop(it)) {
      std::unique_ptr<WsTxItem> item(it);
      if (!item) continue;
      to.clear();
      {
        ClientsLock g(_clientsMx);
        if (!g) {
          lockTimeout("tx");
          continue;
        }
        if (item->cid == 0) {
          // Broadcast to every client SUBSCRIBED to this channel.
          for (auto& kv : _clients) {
            if (kv.second.subs.find(item->channel) != kv.second.subs.end()) to.push_back(kv.first);
          }
        } else if (_clients.count(item->cid)) {
          to.push_back(item->cid);  // unicast (snapshot-on-subscribe)
        }
      }
      for (uint32_t cid : to) sendFrame(cid, *item);
    }
  }

  void sendFrame(uint32_t cid, const WsTxItem& item) {
    AsyncWebSocketClient* c = pin(cid);
    if (!c) return;
    int8_t sent = -1;  // -1 not connected, 0 skipped (back-pressure), 1 queued
    if (c->status() == WS_CONNECTED) {
      if (!c->canSend() || c->queueLen() >= WS_BACKPRESSURE_THRESHOLD) {
        sent = 0;
      } else {
        if (item.text) c->text(item.payload);
        else c->binary(reinterpret_cast<const uint8_t*>(item.payload.c_str()), item.payload.length());
        sent = 1;
      }
    }
    unpin(cid);
    if (sent < 0) return;
    const uint32_t nowMs = millis();
    ClientsLock g(_clientsMx);
    if (!g) {
      lockTimeout("tx");
      return;
    }
    auto cit = _clients.find(cid);
    if (cit == _clients.end()) return;
    if (sent) {
      cit->second.lastTxMs = nowMs;
      cit->second.slowSinceMs = 0;
    } else {
      cit->second.droppedFrames++;
      if (!cit->second.slowSinceMs) cit->second.slowSinceMs = nowMs ? nowMs : 1;
    }
  }

  void processRx() {
    WsRxItem* it = nullptr;
    while (_rxQ.pop(it)) {
      if (!it) continue;

      DynamicJsonDocument doc((size_t)MWS_DEFAULT_JSON_CAP + 256);
      DeserializationError err = deserializeJson(doc, it->payload);

      if (!err && doc.is<JsonObject>()) {
        JsonObject root = doc.as<JsonObject>();
        const char* type = root["type"] | "";
        String ch = root["ch"] | "";

        WebAuthLevel level = WebAuthLevel::Public;
        bool canWrite = false;
        ChannelBase* base = ch.length() ? findChannel(ch) : nullptr;
        const bool known = clientRights(it->cid, level, canWrite);

        if (strcmp(type, "vis") == 0) {
          // The page is shown or hidden (see the cap note at the top).
          ClientsLock g(_clientsMx);
          auto cit = g ? _clients.find(it->cid) : _clients.end();
          if (!g) lockTimeout("vis");
          if (cit != _clients.end()) {
            const int8_t h = (root["hidden"] | false) ? 1 : 0;
            if (h && cit->second.hidden != 1) cit->second.hiddenSinceMs = millis();
            cit->second.hidden = h;
          }
        } else if (ch.length() > 0) {
          if (strcmp(type, "sub") == 0) {
            // Subscribing to an unknown channel stays allowed (no data
            // flows until it exists); a known channel needs the level.
            if (!known || (base && !levelAtLeast(level, base->level))) {
              log_w("[ws] sub denied: cid=%u ch=%s", (unsigned)it->cid, ch.c_str());
            } else {
              uint32_t victim = 0;
              bool subbed = false;
              {
                ClientsLock g(_clientsMx);
                auto cit = g ? _clients.find(it->cid) : _clients.end();
                if (!g) lockTimeout("sub");  // the page subscribes again on its next reconnect
                if (cit != _clients.end()) {
                  cit->second.subs.insert(ch);
                  subbed = true;
                  if (liveCount(&ch) > ESPRACK_WS_MAX_PER_CHANNEL) victim = pickEviction(&ch, it->cid);
                  if (victim == it->cid) cit->second.subs.erase(ch);
                }
              }
              evict(victim, victim == it->cid, "over the per-channel cap");
              if (subbed && victim != it->cid) pushSnapshot(ch, it->cid);   // a unicast; processTx (below) sends it
            }
          } else if (strcmp(type, "unsub") == 0) {
            ClientsLock g(_clientsMx);
            auto cit = g ? _clients.find(it->cid) : _clients.end();
            if (!g) lockTimeout("unsub");
            if (cit != _clients.end()) cit->second.subs.erase(ch);
          } else if (base && root["p"].is<JsonObject>() &&
                     (!known || !canWrite ||
                      !levelAtLeast(level, base->writeLevel
                                               ? base->writeLevel(root["p"].as<JsonObjectConst>())
                                               : base->level))) {
            log_w("[ws] write denied: cid=%u ch=%s", (unsigned)it->cid, ch.c_str());
          } else {
            // State write from the client.
            if (base && root["p"].is<JsonObject>()) {
              String origin = root["origin_id"] | "";
              if (origin.length() == 0) origin = String("ws:") + String(it->cid);
              JsonObject p = root["p"].as<JsonObject>();
              (void)base->apply(p, origin);
            }
          }
        }
      }

      delete it;
    }
  }

  // async_tcp. Roster work under the lock (with a timeout); a frame goes to
  // the rx queue and the loop task pumps it. Sends here go to this event's
  // own client (alive for the event) or to a pinned one (evict).
  void handleWsEvent(AsyncWebSocketClient* c,
                     AwsEventType t,
                     void* a,
                     uint8_t* data,
                     size_t len) {
    const uint32_t id = c->id();
    switch (t) {
      case WS_EVT_CONNECT:
        // `a` is the upgrade request, already DELETED by AsyncWebSocketClient's
        // constructor — never dereference it.
        onConnect(c);
        break;

      case WS_EVT_PONG: {
        ClientsLock g(_clientsMx);
        if (!g) {
          lockTimeout("pong");
          break;
        }
        _lastPong[id] = millis();
        auto cit = _clients.find(id);
        if (cit != _clients.end()) cit->second.lastRxMs = millis();
        if (_lastPingTs) updRtt(millis() - _lastPingTs);
      } break;

      case WS_EVT_DISCONNECT:
        // Fired from the client's destructor: wait for a send in flight to it.
        releaseClient(id);
        forget(id);
        break;

      case WS_EVT_ERROR:
        // A close frame with an error code; the client lives on until its
        // disconnect but gets no more frames.
        forget(id);
        break;

      case WS_EVT_DATA: {
        {
          ClientsLock g(_clientsMx);
          if (g) {
            _lastPong[id] = millis();
            auto cit = _clients.find(id);
            if (cit != _clients.end()) cit->second.lastRxMs = cit->second.lastAppRxMs = millis();
          } else {
            lockTimeout("rx");  // the frame is queued all the same
          }
        }
        AwsFrameInfo* info = reinterpret_cast<AwsFrameInfo*>(a);
        if (info && info->final && info->index == 0 && info->opcode == WS_TEXT) {
          String s(reinterpret_cast<char*>(data), len);
          enqueueRx(id, s, true);  // wakes the loop task
        }
      } break;

      default:
        break;
    }
  }

  // Lock busy: the roster entry stays until purgeGone (after the disconnect).
  void forget(uint32_t id) {
    ClientsLock g(_clientsMx);
    if (!g) {
      lockTimeout("disconnect");
      return;
    }
    _lastPong.erase(id);
    _clients.erase(id);
  }

  void onConnect(AsyncWebSocketClient* c) {
    const uint32_t id = c->id();
    portENTER_CRITICAL(&_pinMx);
    const bool slot = _pins.add(id, c);
    portEXIT_CRITICAL(&_pinMx);

    // The connection's identity was resolved at the handshake (stashIdentity).
    // No manager = open device (admin + operator).
    WebAuthLevel connLevel = WebAuthLevel::Admin;
    bool connCanWrite = true;
    uint32_t victim = 0;
    std::vector<uint32_t> replaced;  // this tab's older sockets
    bool rostered = false;
    if (slot) {
      ClientsLock g(_clientsMx);
      if (!g) {
        lockTimeout("connect");
      } else {
        _lastPong[id] = millis();
        WsClientInfo info;
        info.id = id;
        info.remoteIp = c->remoteIP().toString();
        info.connectedAtMs = millis();
        info.lastRxMs = millis();
        info.lastAppRxMs = millis();
        info.lastTxMs = 0;
        takeIdentity(c->client(), connLevel, connCanWrite, info.tab);
        info.level = connLevel;
        info.canWrite = connCanWrite;
        if (info.tab) {
          for (auto& kv : _clients) {
            if (kv.second.tab != info.tab || !isLive(kv)) continue;
            kv.second.evicting = true;
            replaced.push_back(kv.first);
          }
        }
        _clients[id] = info;   // empty subs — state flows only after an explicit sub
        rostered = true;
        if (liveCount() > ESPRACK_WS_MAX_CLIENTS) victim = pickEviction(nullptr, id);
        if (liveCount() > _peakClients) _peakClients = liveCount();
      }
    }
    if (!rostered) {
      // No pin slot or no roster entry: it could never subscribe. Refuse it.
      ++_refusals;
      log_w("[ws] client %u refused: %s", (unsigned)id, slot ? "roster busy" : "no slot");
      c->close(1000, "busy");
      return;
    }
    for (uint32_t old : replaced) evict(old, false, "its tab reconnected");
    evict(victim, victim == id, "over the client cap");
    if (victim != id) sendId(c);   // {"type":"id","id":"ws:<cid>"} — NO auto state broadcast; client subscribes
  }

  void sendId(AsyncWebSocketClient* c) {
    StaticJsonDocument<64> d;
    d["type"] = "id";
    d["id"] = String("ws:") + String(c->id());
    String s;
    serializeJson(d, s);
    c->text(s);
  }

  // Every open client, pinned one at a time (no lock held while pinging).
  void pingAll() {
    _lastPingTs = millis();
    uint32_t ids[kPinSlots];
    portENTER_CRITICAL(&_pinMx);
    const size_t n = _pins.ids(ids, kPinSlots);
    portEXIT_CRITICAL(&_pinMx);
    for (size_t i = 0; i < n; ++i) {
      AsyncWebSocketClient* c = pin(ids[i]);
      if (!c) continue;
      c->ping();  // a no-op unless connected
      unpin(ids[i]);
    }
  }

  void checkInactive() {
    const unsigned long now = millis();
    std::vector<uint32_t> expired;
    {
      ClientsLock g(_clientsMx);
      if (!g) {
        lockTimeout("pong check");
        return;
      }
      for (auto it = _lastPong.begin(); it != _lastPong.end();) {
        if (now - it->second > _pongTimeoutMs) {
          expired.push_back(it->first);
          it = _lastPong.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (uint32_t cid : expired) closeClient(cid);
  }

  // See WS_HEARTBEAT_MS. A beat is skipped for a client whose queue is full;
  // one that stays full for WS_STUCK_MS is aborted (a close frame would only
  // queue behind the stuck data). Sends go to pinned clients, no lock held.
  void heartbeat() {
    if (!_ws) return;
    const uint32_t now = millis();
    uint32_t ids[kPinSlots];
    portENTER_CRITICAL(&_pinMx);
    const size_t n = _pins.ids(ids, kPinSlots);
    portEXIT_CRITICAL(&_pinMx);
    for (size_t i = 0; i < n; ++i) {
      const uint32_t cid = ids[i];
      AsyncWebSocketClient* c = pin(cid);
      if (!c) continue;
      const bool on = c->status() == WS_CONNECTED;
      const bool slow = on && (!c->canSend() || c->queueLen() >= WS_BACKPRESSURE_THRESHOLD);
      bool stuck = false;
      if (on) {
        ClientsLock g(_clientsMx);
        auto cit = g ? _clients.find(cid) : _clients.end();
        if (!g) lockTimeout("heartbeat");
        if (cit != _clients.end()) {
          WsClientInfo& ci = cit->second;
          if (!slow) ci.slowSinceMs = 0;
          else if (!ci.slowSinceMs) ci.slowSinceMs = now ? now : 1;
          else stuck = now - ci.slowSinceMs >= WS_STUCK_MS;
        }
      }
      if (on && !slow) c->text("{\"type\":\"hb\"}");
      if (stuck) {
        ++_stuckEvictions;
        log_w("[ws] client %u: queue stuck for %u s, dropping the connection", (unsigned)cid,
              (unsigned)(WS_STUCK_MS / 1000));
        if (AsyncClient* tcp = c->client()) tcp->abort();
        else c->close();
      }
      unpin(cid);
    }
  }

  void closeClient(uint32_t cid) {
    if (AsyncWebSocketClient* c = pin(cid)) {
      c->close();
      unpin(cid);
    }
  }

  void updRtt(unsigned long rttMs) {
    if (!_hasFirstRtt) {
      _avgRtt = (float)rttMs;
      _hasFirstRtt = true;
      return;
    }
    _avgRtt = _alpha * (float)rttMs + (1.0f - _alpha) * _avgRtt;
  }
};
