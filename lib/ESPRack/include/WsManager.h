#pragma once

// Multi-endpoint WebSocket manager for ESPAsyncWebServer.
//
// Цілі:
// - ConfigManager-like wiring style через WsManager::Binding<T>
// - Коректний доступ до стану через StatefulService<T>::read / update
// - Черги без жорсткої привʼязки до FreeRTOS (але з коректною синхронізацією на ESP32)

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Ticker.h>

#include <functional>
#include <map>
#include <memory>
#include <vector>

#ifdef ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif

#include "StatefulService.h"

#ifndef MWS_DEFAULT_JSON_CAP
#define MWS_DEFAULT_JSON_CAP 2048
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
struct WsTxItem {
  String path;
  uint32_t cid;
  String payload;
  bool text;
};

struct WsRxItem {
  String path;
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
        _avgRtt(0.0f) {}

  ~WsManager() {
    _pingTicker.detach();
    _endpoints.clear();
    _endpointsByPath.clear();
    _lastPong.clear();
  }

  // =======================================================================
  // ConfigManager-style helper: Binding<TState>
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

      // Автобродкаст при будь-якому апдейті сервісу (REST/MQTT/WS),
      // origin передається, щоб фронт міг відсіяти власні echo.
      if (_autoBroadcast && !_handlerId) {
        _handlerId = addHandlerCompat(
            _service,
            [this](const String& origin) {
              if (_mgr) _mgr->broadcastCurrentState(_path, origin);
            },
            false /* як у твоєму коді: без "початкового" виклику */,
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

    // Підтримка двох підписів addUpdateHandler:
    // 1) addUpdateHandler(fn)
    // 2) addUpdateHandler(fn, bool)
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
  // Endpoint registration
  // =======================================================================
template <typename TState>
void* addEndpoint(const String& path,
                  StatefulService<TState>* svc,
                  JsonStateReader<TState> reader,
                  JsonStateUpdater<TState> updater,
                  size_t jsonCapacity = MWS_DEFAULT_JSON_CAP) {
  if (!_server || !svc || path.length() == 0) {
    return nullptr;
  }

  if (_endpointsByPath.find(path) != _endpointsByPath.end()) {
    return _endpointsByPath[path];
  }

  std::unique_ptr<Endpoint<TState>> ep(new Endpoint<TState>(path, svc, reader, updater, jsonCapacity));
  EndpointBase* epPtr = ep.get();

  AsyncWebSocket* ws = new AsyncWebSocket(path.c_str());
  epPtr->ws = ws;

  ws->onEvent([this, epPtr](AsyncWebSocket* /*s*/, AsyncWebSocketClient* c, AwsEventType t, void* a, uint8_t* d,
                            size_t l) {
    if (!epPtr || !c) return;
    this->handleWsEvent(*epPtr, c, t, a, d, l);
  });

  _server->addHandler(ws);

  _endpointsByPath[path] = epPtr;
  _endpoints.push_back(std::move(ep));

  return epPtr;
}

  // =======================================================================
  // Sending API
  // =======================================================================
  void broadcast(const String& path, const String& payload, bool text = true) { enqueueTx(path, 0, payload, text); }

  void sendTo(const String& path, uint32_t cid, const String& payload, bool text = true) {
    enqueueTx(path, cid, payload, text);
  }

  // Subscriber introspection — used by services to gate expensive work
  // (periodic ticks, per-message echoes) before building any payload.
  bool hasClients(const String& path) const {
    auto it = _endpointsByPath.find(path);
    if (it == _endpointsByPath.end() || !it->second || !it->second->ws) return false;
    return it->second->ws->count() > 0;
  }

  size_t clientCount(const String& path) const {
    auto it = _endpointsByPath.find(path);
    if (it == _endpointsByPath.end() || !it->second || !it->second->ws) return 0;
    return it->second->ws->count();
  }

  // Serialise the full registry for the admin Sockets tab: every endpoint
  // with its current client roster (id, ip, how long each has been around,
  // seconds since last inbound/outbound frame). Safe to call from any
  // context that already holds an AsyncJsonResponse.
  void fillStats(JsonObject& root) const {
    const unsigned long now = millis();
    auto sinceSec = [now](uint32_t ts) -> uint32_t {
      if (ts == 0) return 0;
      return (uint32_t)((now - ts) / 1000UL);
    };

    JsonArray eps = root.createNestedArray("endpoints");
    size_t total = 0;
    uint32_t totalDropped = 0;
    uint32_t totalBroadcastsDropped = 0;
    for (auto& ep : _endpoints) {
      if (!ep || !ep->ws) continue;
      JsonObject j = eps.createNestedObject();
      j["path"]  = ep->path;
      j["count"] = ep->ws->count();
      j["dropped_broadcasts"] = ep->droppedBroadcasts;
      total += ep->ws->count();
      totalBroadcastsDropped += ep->droppedBroadcasts;
      JsonArray cl = j.createNestedArray("clients");
      for (auto& kv : ep->clients) {
        JsonObject c = cl.createNestedObject();
        c["id"]         = kv.second.id;
        c["ip"]         = kv.second.remoteIp;
        c["age_s"]      = sinceSec(kv.second.connectedAtMs);
        c["last_rx_s"]  = sinceSec(kv.second.lastRxMs);
        c["last_tx_s"]  = kv.second.lastTxMs == 0 ? (uint32_t)0 : sinceSec(kv.second.lastTxMs);
        c["drops"]      = kv.second.droppedFrames;
        totalDropped += kv.second.droppedFrames;
      }
    }
    root["total_clients"]  = total;
    root["avg_rtt_ms"]     = _avgRtt;
    root["ping_interval_s"] = _pingIntervalSec;
    root["pong_timeout_ms"] = _pongTimeoutMs;
    root["total_dropped"]  = totalDropped;
    root["total_broadcasts_dropped"] = totalBroadcastsDropped;
    root["backpressure_threshold"]   = (uint32_t)WS_BACKPRESSURE_THRESHOLD;
    root["backpressure_max"]         = (uint32_t)WS_MAX_QUEUED_MESSAGES;
  }

  // Push current state
  void broadcastCurrentState(const String& path, const String& origin = "") {
    EndpointBase* base = findEndpoint(path);
    if (!base) return;
    // Fallback guard: skip the full JSON allocation + readInto + serialise
    // when nobody's subscribed. Services should short-circuit even earlier
    // via WebFeatureEntry::hasSubscribers() so we don't even get here, but
    // keeping this as belt-and-suspenders for any caller that forgot.
    if (!base->ws || base->ws->count() == 0) return;
    // If every client is currently back-pressured, skip the build-once step
    // entirely — the payload would land in the queue only to be dropped by
    // processTx anyway. Accounted on the endpoint rather than individual
    // clients because none even had the frame offered to them.
    bool anyReady = false;
    for (auto& client : base->ws->getClients()) {
      if (client.canSend() && client.queueLen() < WS_BACKPRESSURE_THRESHOLD) {
        anyReady = true;
        break;
      }
    }
    if (!anyReady) {
      base->droppedBroadcasts++;
      return;
    }

    const size_t cap = base->jsonCapacity > 0 ? base->jsonCapacity : (size_t)MWS_DEFAULT_JSON_CAP;
    DynamicJsonDocument doc(cap + 256);

    JsonObject root = doc.to<JsonObject>();
    root["type"] = "p";
    root["origin_id"] = origin;
    JsonObject p = root.createNestedObject("p");

    base->readInto(p);

    String out;
    serializeJson(doc, out);
    broadcast(path, out, true);
  }

  // Processing queues (call from loop)
  void processAllQueues() {
    // clean up stale WS connections to free TCP slots for HTTP
    for (auto& ep : _endpoints) {
      if (ep && ep->ws) ep->ws->cleanupClients(2);
    }
    processRx();
    processTx();
  }

  // Ping/Pong keepalive
  void beginPingPong(unsigned pingIntervalSec = 10, unsigned long pongTimeoutMs = 30000) {
    _pingIntervalSec = pingIntervalSec;
    _pongTimeoutMs = pongTimeoutMs;
    _pingTicker.detach();
    _pingTicker.attach((float)_pingIntervalSec, &WsManager::staticPingCb, this);
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

  // Per-client bookkeeping for diagnostics and phantom-eviction. Populated
  // by handleWsEvent on connect/disconnect/rx/pong; read by fillStats() for
  // the admin Sockets tab.
  struct WsClientInfo {
    uint32_t id{0};
    String   remoteIp;
    uint32_t connectedAtMs{0};
    uint32_t lastRxMs{0};
    uint32_t lastTxMs{0};
    uint32_t droppedFrames{0};  // frames skipped due to per-client back-pressure
  };

  // --------- Endpoint type-erasure ---------
  struct EndpointBase {
    String path;
    AsyncWebSocket* ws{nullptr};
    size_t jsonCapacity{MWS_DEFAULT_JSON_CAP};
    std::map<uint32_t, WsClientInfo> clients;
    // Broadcasts that were skipped entirely because every client was
    // back-pressured at the time — we saved a full serialise cycle.
    uint32_t droppedBroadcasts{0};

    virtual ~EndpointBase() {
      if (ws) {
        delete ws;
        ws = nullptr;
      }
    }

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
  struct Endpoint : public EndpointBase {
    Endpoint(const String& p,
             StatefulService<TState>* s,
             JsonStateReader<TState> r,
             JsonStateUpdater<TState> u,
             size_t cap)
        : svc(s), reader(r), updater(u) {
      path = p;
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

  std::vector<std::unique_ptr<EndpointBase>> _endpoints;
  std::map<String, EndpointBase*> _endpointsByPath;

  PtrRingQueue<WsTxItem> _txQ;
  PtrRingQueue<WsRxItem> _rxQ;

  // ping/pong
  Ticker _pingTicker;
  unsigned _pingIntervalSec;
  unsigned long _pongTimeoutMs;
  unsigned long _lastPingTs;
  std::map<uint32_t, unsigned long> _lastPong;

  // RTT
  bool _hasFirstRtt;
  float _alpha;
  float _avgRtt;

  EndpointBase* findEndpoint(const String& path) {
    auto it = _endpointsByPath.find(path);
    if (it == _endpointsByPath.end()) return nullptr;
    return it->second;
  }

  void enqueueTx(const String& path, uint32_t cid, const String& payload, bool text) {
    auto* it = new WsTxItem{path, cid, payload, text};
    if (!_txQ.push(it)) delete it;
  }

  void enqueueRx(const String& path, uint32_t cid, const String& payload, bool text) {
    auto* it = new WsRxItem{path, cid, payload, text};
    if (!_rxQ.push(it)) delete it;
  }

  void processTx() {
    WsTxItem* it = nullptr;
    while (_txQ.pop(it)) {
      if (!it) continue;

      EndpointBase* base = findEndpoint(it->path);
      if (base && base->ws) {
        const uint32_t nowMs = millis();
        if (it->cid == 0) {
          // Broadcast — but dispatch per-client so a single slow client
          // can't fill its own queue to the WS_MAX_QUEUED_MESSAGES ceiling
          // and trigger AsyncWebSocket's "too many queued" close. Each
          // client is gated on canSend() + queueLen() < threshold; failures
          // are counted per-client so the admin Sockets tab can pinpoint
          // the straggler while fast clients keep receiving every frame.
          for (auto& client : base->ws->getClients()) {
            const uint32_t cid = client.id();
            auto cit = base->clients.find(cid);
            const bool slow = !client.canSend() || client.queueLen() >= WS_BACKPRESSURE_THRESHOLD;
            if (slow) {
              if (cit != base->clients.end()) cit->second.droppedFrames++;
              continue;
            }
            if (it->text) {
              client.text(it->payload);
            } else {
              client.binary(reinterpret_cast<const uint8_t*>(it->payload.c_str()), it->payload.length());
            }
            if (cit != base->clients.end()) cit->second.lastTxMs = nowMs;
          }
        } else {
          // Unicast — same gate; if the specific client is back-pressured
          // we drop and increment its counter rather than risk the close.
          AsyncWebSocketClient* c = base->ws->client(it->cid);
          if (c && c->status() == WS_CONNECTED) {
            auto cit = base->clients.find(it->cid);
            const bool slow = !c->canSend() || c->queueLen() >= WS_BACKPRESSURE_THRESHOLD;
            if (slow) {
              if (cit != base->clients.end()) cit->second.droppedFrames++;
            } else {
              if (it->text) c->text(it->payload);
              else c->binary(reinterpret_cast<const uint8_t*>(it->payload.c_str()), it->payload.length());
              if (cit != base->clients.end()) cit->second.lastTxMs = nowMs;
            }
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

      EndpointBase* base = findEndpoint(it->path);
      if (!base) {
        delete it;
        continue;
      }

      DynamicJsonDocument doc((base->jsonCapacity ? base->jsonCapacity : (size_t)MWS_DEFAULT_JSON_CAP) + 256);
      DeserializationError err = deserializeJson(doc, it->payload);

      if (!err && doc.is<JsonObject>()) {
        JsonObject root = doc.as<JsonObject>();

        // origin: або з повідомлення, або дефолт "ws:<cid>"
        String origin;
        if (root.containsKey("origin_id")) origin = root["origin_id"].as<String>();
        if (origin.length() == 0) origin = String("ws:") + String(it->cid);

        if (root.containsKey("p") && root["p"].is<JsonObject>()) {
          JsonObject p = root["p"].as<JsonObject>();
          (void)base->apply(p, origin);
        }
      }

      delete it;
    }
  }

  void handleWsEvent(EndpointBase& ep,
                     AsyncWebSocketClient* c,
                     AwsEventType t,
                     void* a,
                     uint8_t* data,
                     size_t len) {
    const uint32_t id = c->id();

    switch (t) {
      case WS_EVT_CONNECT: {
        _lastPong[id] = millis();
        WsClientInfo info;
        info.id = id;
        info.remoteIp = c->remoteIP().toString();
        info.connectedAtMs = millis();
        info.lastRxMs = millis();
        info.lastTxMs = 0;
        ep.clients[id] = info;
        sendId(c);
        // одразу пушимо стан endpoint-а
        broadcastCurrentState(ep.path, "ws_connect");
      } break;

      case WS_EVT_PONG: {
        _lastPong[id] = millis();
        auto itPong = ep.clients.find(id);
        if (itPong != ep.clients.end()) itPong->second.lastRxMs = millis();
        if (_lastPingTs) updRtt(millis() - _lastPingTs);
      } break;

      case WS_EVT_DISCONNECT:
      case WS_EVT_ERROR:
        _lastPong.erase(id);
        ep.clients.erase(id);
        break;

      case WS_EVT_DATA: {
        // Any inbound frame proves the TCP link is alive, so count it as a
        // pong-equivalent — protects active clients from being evicted just
        // because they missed a scheduled pong window.
        _lastPong[id] = millis();
        auto itData = ep.clients.find(id);
        if (itData != ep.clients.end()) itData->second.lastRxMs = millis();
        AwsFrameInfo* info = reinterpret_cast<AwsFrameInfo*>(a);
        if (info && info->final && info->index == 0 && info->opcode == WS_TEXT) {
          String s(reinterpret_cast<char*>(data), len);
          enqueueRx(ep.path, id, s, true);
        }
      } break;

      default:
        break;
    }

    // одразу обробляємо — швидкий UX
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

  static void staticPingCb(WsManager* m) {
    if (m) {
      m->pingAll();
      m->checkInactive();
    }
  }

  void pingAll() {
    _lastPingTs = millis();
    for (auto& up : _endpoints) {
      if (up && up->ws) up->ws->pingAll();
    }
  }

  void checkInactive() {
    const unsigned long now = millis();
    for (auto it = _lastPong.begin(); it != _lastPong.end();) {
      if (now - it->second > _pongTimeoutMs) {
        closeAll(it->first);
        it = _lastPong.erase(it);
      } else {
        ++it;
      }
    }
  }

  void closeAll(uint32_t cid) {
    for (auto& up : _endpoints) {
      if (!up || !up->ws) continue;
      if (AsyncWebSocketClient* c = up->ws->client(cid)) c->close();
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
