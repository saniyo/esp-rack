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

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#ifdef ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif

#include "StatefulService.h"

#ifndef MWS_DEFAULT_JSON_CAP
#define MWS_DEFAULT_JSON_CAP 2048
#endif

// The single multiplexed WebSocket path. Distinct from /ws/presence (which stays
// its own always-on socket) so there is no path-prefix ambiguity. Override via -D.
#ifndef WS_MUX_PATH
#define WS_MUX_PATH "/ws/app"
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
    // (processAllQueues from App::loop). Recursive so a locked path that
    // re-enters (e.g. event -> processAllQueues -> processTx) can't self-deadlock.
    _clientsMx = xSemaphoreCreateRecursiveMutex();
    // Guards the ONE shared outbound-build doc (_txDoc), reused for every
    // channel's state frame instead of a per-broadcast allocation.
    _txDocMx = xSemaphoreCreateRecursiveMutex();
  }

  ~WsManager() {
    _channelList.clear();
    _channels.clear();
    _clients.clear();
    _lastPong.clear();
    if (_ws) { delete _ws; _ws = nullptr; }
    if (_clientsMx) vSemaphoreDelete(_clientsMx);
    if (_txDocMx) vSemaphoreDelete(_txDocMx);
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
            bool autoBroadcast = true)
        : _mgr(manager),
          _path(path ? String(path) : String()),
          _service(service),
          _jsonCapacity(jsonCapacity ? jsonCapacity : (size_t)MWS_DEFAULT_JSON_CAP),
          _reader(reader),
          _updater(updater),
          _autoBroadcast(autoBroadcast) {
      attach();
    }

    void attach() {
      if (_attached) return;
      if (!_mgr || !_service || _path.length() == 0) return;

      _mgr->template addEndpoint<TState>(_path, _service, _reader, _updater, _jsonCapacity);

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

  // `path` is the CHANNEL name (feature id). All channels share one socket.
  template <typename TState>
  void* addEndpoint(const String& path,
                    StatefulService<TState>* svc,
                    JsonStateReader<TState> reader,
                    JsonStateUpdater<TState> updater,
                    size_t jsonCapacity = MWS_DEFAULT_JSON_CAP) {
    if (!_server || !svc || path.length() == 0) {
      return nullptr;
    }

    ensureSocket();

    auto existing = _channels.find(path);
    if (existing != _channels.end()) {
      return existing->second;
    }

    std::unique_ptr<Channel<TState>> ch(new Channel<TState>(path, svc, reader, updater, jsonCapacity));
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

  // Serialise the registry for the admin Clients/Endpoints tabs: every channel
  // with the clients currently SUBSCRIBED to it (id, ip, age, seconds since last
  // rx/tx). Shape kept compatible with the old per-endpoint layout
  // (endpoints[].clients[]) so WebEndpointsService/WsDiag need no change.
  void fillStats(JsonObject& root) const {
    ClientsLock g(_clientsMx);
    const unsigned long now = millis();
    auto sinceSec = [now](uint32_t ts) -> uint32_t {
      if (ts == 0) return 0;
      return (uint32_t)((now - ts) / 1000UL);
    };

    JsonArray eps = root.createNestedArray("endpoints");
    uint32_t totalBroadcastsDropped = 0;
    for (auto& chPair : _channels) {
      ChannelBase* ch = chPair.second;
      if (!ch) continue;
      JsonObject j = eps.createNestedObject();
      j["path"] = ch->name;
      totalBroadcastsDropped += ch->droppedBroadcasts;
      j["dropped_broadcasts"] = ch->droppedBroadcasts;
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
      }
      j["count"] = cnt;
    }

    uint32_t totalDropped = 0;
    for (auto& kv : _clients) totalDropped += kv.second.droppedFrames;

    root["total_clients"]  = _clients.size();   // distinct connections on the one socket
    root["avg_rtt_ms"]     = _avgRtt;
    root["ping_interval_s"] = _pingIntervalSec;
    root["pong_timeout_ms"] = _pongTimeoutMs;
    root["total_dropped"]  = totalDropped;
    root["total_broadcasts_dropped"] = totalBroadcastsDropped;
    root["backpressure_threshold"]   = (uint32_t)WS_BACKPRESSURE_THRESHOLD;
    root["backpressure_max"]         = (uint32_t)WS_MAX_QUEUED_MESSAGES;
  }

  // Push current channel state to ALL its subscribers.
  void broadcastCurrentState(const String& channel, const String& origin = "") {
    ChannelBase* base = findChannel(channel);
    if (!base || !_ws) return;
    // Belt-and-suspenders: skip the serialise when nobody is subscribed.
    // Services should also gate earlier via hasClients(channel).
    if (subscriberCount(channel) == 0) return;
    String out;
    if (!buildStateFrame(base, origin, out)) return;
    broadcast(channel, out, true);
  }

  // Processing queues (call from loop)
  void processAllQueues() {
    if (_ws) _ws->cleanupClients(2);  // free TCP slots for HTTP
    processRx();
    processTx();
    // Keepalive — inline on the queue-pump task so ping/eviction share the
    // same context (and lock) as the map mutations. Gated by interval.
    if (_pingIntervalSec &&
        (millis() - _lastPingTs >= (unsigned long)_pingIntervalSec * 1000UL)) {
      pingAll();          // updates _lastPingTs
      checkInactive();
    }
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
    portMUX_TYPE _mx = portMUX_INITIALIZER_UNLOCKED;
    void lock() { portENTER_CRITICAL(&_mx); }
    void unlock() { portEXIT_CRITICAL(&_mx); }
#else
    void lock() { noInterrupts(); }
    void unlock() { interrupts(); }
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
    uint32_t lastTxMs{0};
    uint32_t droppedFrames{0};   // frames skipped due to per-client back-pressure
    std::set<String> subs;       // channels this client subscribed to
  };

  // --------- Channel type-erasure ---------
  struct ChannelBase {
    String name;                              // channel id (== feature id)
    size_t jsonCapacity{MWS_DEFAULT_JSON_CAP};
    uint32_t droppedBroadcasts{0};            // skipped because no subscriber was ready

    virtual ~ChannelBase() {}
    virtual void readInto(JsonObject& out) = 0;
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

    void readInto(JsonObject& out) override {
      if (!svc) return;
      svc->read(out, reader);
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

  SemaphoreHandle_t _clientsMx = nullptr;
  struct ClientsLock {
    SemaphoreHandle_t h;
    explicit ClientsLock(SemaphoreHandle_t s) : h(s) {
      if (h) xSemaphoreTakeRecursive(h, portMAX_DELAY);
    }
    ~ClientsLock() { if (h) xSemaphoreGiveRecursive(h); }
    ClientsLock(const ClientsLock&) = delete;
    ClientsLock& operator=(const ClientsLock&) = delete;
  };

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
    if (_authFilter) _ws->setFilter(_authFilter);
    _server->addHandler(_ws);
  }

  ChannelBase* findChannel(const String& name) {
    auto it = _channels.find(name);
    return (it == _channels.end()) ? nullptr : it->second;
  }

  size_t subscriberCount(const String& channel) const {
    ClientsLock g(_clientsMx);
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
  bool buildStateFrame(ChannelBase* base, const String& origin, String& out) {
    if (!base) return false;
    ClientsLock g(_txDocMx);
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
    base->readInto(p);
    serializeJson(*_txDoc, out);
    return true;
  }

  // Snapshot-on-subscribe: push the channel's current state to ONE client.
  void pushSnapshot(const String& channel, uint32_t cid) {
    ChannelBase* base = findChannel(channel);
    if (!base) return;
    String out;
    if (!buildStateFrame(base, "ws_sub", out)) return;
    sendTo(channel, cid, out, true);
  }

  void enqueueTx(const String& channel, uint32_t cid, const String& payload, bool text) {
    auto* it = new WsTxItem{channel, cid, payload, text};
    if (!_txQ.push(it)) delete it;
  }

  void enqueueRx(uint32_t cid, const String& payload, bool text) {
    auto* it = new WsRxItem{cid, payload, text};
    if (!_rxQ.push(it)) delete it;
  }

  void processTx() {
    // Reads/updates the _clients roster per frame; lock so a WS
    // connect/disconnect (async_tcp) can't erase an entry mid-iteration.
    ClientsLock g(_clientsMx);
    if (!_ws) {
      WsTxItem* drain = nullptr;
      while (_txQ.pop(drain)) delete drain;
      return;
    }
    WsTxItem* it = nullptr;
    const uint32_t nowMs = millis();
    while (_txQ.pop(it)) {
      if (!it) continue;
      if (it->cid == 0) {
        // Broadcast to every client SUBSCRIBED to this channel.
        for (auto& kv : _clients) {
          if (kv.second.subs.find(it->channel) == kv.second.subs.end()) continue;
          AsyncWebSocketClient* c = _ws->client(kv.first);
          if (!c || c->status() != WS_CONNECTED) continue;
          const bool slow = !c->canSend() || c->queueLen() >= WS_BACKPRESSURE_THRESHOLD;
          if (slow) { kv.second.droppedFrames++; continue; }
          if (it->text) c->text(it->payload);
          else c->binary(reinterpret_cast<const uint8_t*>(it->payload.c_str()), it->payload.length());
          kv.second.lastTxMs = nowMs;
        }
      } else {
        // Unicast (snapshot-on-subscribe).
        AsyncWebSocketClient* c = _ws->client(it->cid);
        if (c && c->status() == WS_CONNECTED) {
          auto cit = _clients.find(it->cid);
          const bool slow = !c->canSend() || c->queueLen() >= WS_BACKPRESSURE_THRESHOLD;
          if (slow) {
            if (cit != _clients.end()) cit->second.droppedFrames++;
          } else {
            if (it->text) c->text(it->payload);
            else c->binary(reinterpret_cast<const uint8_t*>(it->payload.c_str()), it->payload.length());
            if (cit != _clients.end()) cit->second.lastTxMs = nowMs;
          }
        }
      }
      delete it;
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

        if (ch.length() > 0) {
          if (strcmp(type, "sub") == 0) {
            {
              ClientsLock g(_clientsMx);
              auto cit = _clients.find(it->cid);
              if (cit != _clients.end()) cit->second.subs.insert(ch);
            }
            pushSnapshot(ch, it->cid);   // enqueues a unicast; processTx (below) sends it
          } else if (strcmp(type, "unsub") == 0) {
            ClientsLock g(_clientsMx);
            auto cit = _clients.find(it->cid);
            if (cit != _clients.end()) cit->second.subs.erase(ch);
          } else {
            // State write from the client.
            ChannelBase* base = findChannel(ch);
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

  void handleWsEvent(AsyncWebSocketClient* c,
                     AwsEventType t,
                     void* a,
                     uint8_t* data,
                     size_t len) {
    const uint32_t id = c->id();
    bool justConnected = false;

    // Map mutations under the lock. ws sends (sendId) and queue pumping happen
    // AFTER releasing it.
    {
      ClientsLock g(_clientsMx);
      switch (t) {
        case WS_EVT_CONNECT: {
          _lastPong[id] = millis();
          WsClientInfo info;
          info.id = id;
          info.remoteIp = c->remoteIP().toString();
          info.connectedAtMs = millis();
          info.lastRxMs = millis();
          info.lastTxMs = 0;
          _clients[id] = info;   // empty subs — state flows only after an explicit sub
          justConnected = true;
        } break;

        case WS_EVT_PONG: {
          _lastPong[id] = millis();
          auto cit = _clients.find(id);
          if (cit != _clients.end()) cit->second.lastRxMs = millis();
          if (_lastPingTs) updRtt(millis() - _lastPingTs);
        } break;

        case WS_EVT_DISCONNECT:
        case WS_EVT_ERROR:
          _lastPong.erase(id);
          _clients.erase(id);
          break;

        case WS_EVT_DATA: {
          _lastPong[id] = millis();
          auto cit = _clients.find(id);
          if (cit != _clients.end()) cit->second.lastRxMs = millis();
          AwsFrameInfo* info = reinterpret_cast<AwsFrameInfo*>(a);
          if (info && info->final && info->index == 0 && info->opcode == WS_TEXT) {
            String s(reinterpret_cast<char*>(data), len);
            enqueueRx(id, s, true);
          }
        } break;

        default:
          break;
      }
    }

    if (justConnected) {
      sendId(c);   // {"type":"id","id":"ws:<cid>"} — NO auto state broadcast; client subscribes
    }

    processAllQueues();
  }

  void sendId(AsyncWebSocketClient* c) {
    StaticJsonDocument<64> d;
    d["type"] = "id";
    d["id"] = String("ws:") + String(c->id());
    String s;
    serializeJson(d, s);
    c->text(s);
  }

  void pingAll() {
    _lastPingTs = millis();
    if (_ws) _ws->pingAll();
  }

  void checkInactive() {
    const unsigned long now = millis();
    std::vector<uint32_t> expired;
    {
      ClientsLock g(_clientsMx);
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

  void closeClient(uint32_t cid) {
    if (!_ws) return;
    if (AsyncWebSocketClient* c = _ws->client(cid)) c->close();
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
