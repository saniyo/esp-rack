#ifndef HttpEndpoint_h
#define HttpEndpoint_h

#include <functional>

#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>

#include <ProblemJson.h>
#include <SecurityManager.h>
#include <StatefulService.h>

// WebManager forward-declared only — circular include otherwise
// (WebManager.h -> WebFeatureDelegate.h -> HttpEndpoint.h -> here).
// registerProxy() below is templated on the WebManager* parameter so
// the call to `web->registerProxyEndpoint(...)` becomes a dependent
// name and lookup defers until instantiation, by which point the
// consumer has already included <WebManager.h>.
class WebManager;

#define HTTP_ENDPOINT_ORIGIN_ID "http"

// Sentinel for registerFeature(..., bufferSize) / WebFeatureEntry: a feature
// that passes WEBFEATURE_BUFFER_AUTO gets its REST buffer sized automatically
// at boot from the REAL ArduinoJson tree (doc.memoryUsage()) instead of a
// hand-guessed worst-case constant. 0 is safe — a 0-byte response buffer is
// never a real request.
static constexpr size_t WEBFEATURE_BUFFER_AUTO = 0;

// ---- Adaptive JSON state response --------------------------------------
// Builds the service's state tree into an AsyncJsonResponse and sends it.
// The configured buffer size is a STARTING POINT, not a contract:
//
//   * Tree overflows the doc (explicit size set too small, or an
//     auto-probed schema grew at runtime)  ->  the doc is grown ×2 and
//     the tree rebuilt, transparently, until it fits or the heap caps
//     out. ArduinoJson otherwise DROPS the overflowing inserts silently
//     and the caller ships a truncated form with a 200 — the worst
//     failure mode of all, because nothing logs and the UI just misses
//     fields.
//   * Heap pressure (configured size momentarily larger than the largest
//     free block) -> the first attempt is CLAMPED DOWN to what the heap
//     can serve right now; a small tree inside a generously-sized buffer
//     still goes out instead of 503ing on the configured constant.
//   * Truly impossible (tree larger than any allocatable doc) -> 503
//     {"error":"heap_pressure","retryAfter":1}; the UI's axios layer
//     auto-retries 503s, so transient pressure self-heals client-side.
//
// The WIRE transfer needs no contiguous output copy in any of these
// cases: AsyncJsonResponse serialises the retained doc window-by-window
// into TCP-sized chunks on every ack (ChunkPrint), so the only thing
// that must fit contiguously is the tree itself.
//
// Returns true if a 2xx response was queued, false after the 503.
template <class T>
inline bool sendJsonStateResponse(AsyncWebServerRequest* request,
                                  StatefulService<T>* service,
                                  const JsonStateReader<T>& reader,
                                  size_t configuredSize) {
  // Slack kept free above the doc so the response head String + TCP
  // bookkeeping never compete with the tree for the same block.
  static constexpr size_t kSlack    = 1024;
  static constexpr size_t kMinDoc   = 1024;
  static constexpr int    kAttempts = 4;

  size_t want = configuredSize ? configuredSize : DEFAULT_BUFFER_SIZE;
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    const size_t ceiling = ESP.getMaxAllocHeap();
    if (ceiling < kMinDoc + kSlack) break;  // hopeless right now — shed
    size_t size = want;
    if (size + kSlack > ceiling) size = ceiling - kSlack;  // pressure-clamp
    if (size < kMinDoc) break;

    AsyncJsonResponse* response = new AsyncJsonResponse(false, size);
    JsonObject root = response->getRoot().to<JsonObject>();
    if (root.isNull()) {  // doc alloc lost the race despite the clamp
      delete response;
      break;
    }
    service->read(root, reader);
    if (!response->overflowed()) {
      response->setLength();
      request->send(response);
      return true;
    }
    // Tree didn't fit this doc — grow and rebuild. If we were already
    // pressure-clamped to the ceiling, growing is impossible: bail to 503.
    delete response;
    size_t next = size * 2;
    if (next + kSlack > ceiling) break;
    log_w("[http.json] tree overflowed %u B doc — retrying at %u B",
          (unsigned)size, (unsigned)next);
    want = next;
  }
  sendProblem(request, 503, "heap_pressure",
              "No contiguous heap for the response right now", 1);
  return false;
}

template <class T>
class HttpGetEndpoint {
 public:
  HttpGetEndpoint(JsonStateReader<T> stateReader,
                  StatefulService<T>* statefulService,
                  AsyncWebServer* server,
                  const String& servicePath,
                  SecurityManager* securityManager,
                  AuthenticationPredicate authenticationPredicate = AuthenticationPredicates::IS_ADMIN,
                  size_t bufferSize = DEFAULT_BUFFER_SIZE) :
      _stateReader(stateReader), _statefulService(statefulService), _bufferSize(bufferSize) {
    server->on(servicePath.c_str(),
               HTTP_GET,
               securityManager->wrapRequest(std::bind(&HttpGetEndpoint::fetchSettings, this, std::placeholders::_1),
                                            authenticationPredicate));
  }

  HttpGetEndpoint(JsonStateReader<T> stateReader,
                  StatefulService<T>* statefulService,
                  AsyncWebServer* server,
                  const String& servicePath,
                  size_t bufferSize = DEFAULT_BUFFER_SIZE) :
      _stateReader(stateReader), _statefulService(statefulService), _bufferSize(bufferSize) {
    server->on(servicePath.c_str(), HTTP_GET, std::bind(&HttpGetEndpoint::fetchSettings, this, std::placeholders::_1));
  }

 protected:
  JsonStateReader<T> _stateReader;
  StatefulService<T>* _statefulService;
  size_t _bufferSize;

  void fetchSettings(AsyncWebServerRequest* request) {
    // All sizing/pressure handling lives in sendJsonStateResponse: the
    // configured _bufferSize is just the starting point — the doc grows
    // when the tree overflows it, shrinks under heap pressure, and only
    // a genuinely impossible build ships the retryable 503.
    sendJsonStateResponse(request, _statefulService, _stateReader, _bufferSize);
  }
};

template <class T>
class HttpPostEndpoint {
 public:
  HttpPostEndpoint(JsonStateReader<T> stateReader,
                   JsonStateUpdater<T> stateUpdater,
                   StatefulService<T>* statefulService,
                   AsyncWebServer* server,
                   const String& servicePath,
                   SecurityManager* securityManager,
                   AuthenticationPredicate authenticationPredicate = AuthenticationPredicates::IS_ADMIN,
                   size_t bufferSize = DEFAULT_BUFFER_SIZE) :
      _stateReader(stateReader),
      _stateUpdater(stateUpdater),
      _statefulService(statefulService),
      _updateHandler(
          servicePath,
          securityManager->wrapCallback(
              std::bind(&HttpPostEndpoint::updateSettings, this, std::placeholders::_1, std::placeholders::_2),
              authenticationPredicate),
          bufferSize),
      _bufferSize(bufferSize) {
    _updateHandler.setMethod(HTTP_POST);
    server->addHandler(&_updateHandler);
  }

  HttpPostEndpoint(JsonStateReader<T> stateReader,
                   JsonStateUpdater<T> stateUpdater,
                   StatefulService<T>* statefulService,
                   AsyncWebServer* server,
                   const String& servicePath,
                   size_t bufferSize = DEFAULT_BUFFER_SIZE) :
      _stateReader(stateReader),
      _stateUpdater(stateUpdater),
      _statefulService(statefulService),
      _updateHandler(servicePath,
                     std::bind(&HttpPostEndpoint::updateSettings, this, std::placeholders::_1, std::placeholders::_2),
                     bufferSize),
      _bufferSize(bufferSize) {
    _updateHandler.setMethod(HTTP_POST);
    server->addHandler(&_updateHandler);
  }

 protected:
  JsonStateReader<T> _stateReader;
  JsonStateUpdater<T> _stateUpdater;
  StatefulService<T>* _statefulService;
  AsyncCallbackJsonWebHandler _updateHandler;
  size_t _bufferSize;

  void updateSettings(AsyncWebServerRequest* request, JsonVariant& json) {
    if (!json.is<JsonObject>()) {
      request->send(400);
      return;
    }
    JsonObject jsonObject = json.as<JsonObject>();
    StateUpdateResult outcome = _statefulService->updateWithoutPropagation(jsonObject, _stateUpdater);
    if (outcome == StateUpdateResult::ERROR) {
      request->send(400);
      return;
    }
    // Echo the post-update state back. sendJsonStateResponse owns all
    // sizing: grows the doc when the tree overflows the configured
    // buffer, shrinks under heap pressure, 503s only when genuinely
    // impossible. The write ALREADY applied via updateWithoutPropagation
    // above — on a 503 the client refetches and state propagates on the
    // next GET, so we also skip the update-handler chain below (same
    // contract as before: saveIfChanged + broadcastWs ride only on a
    // delivered response).
    if (!sendJsonStateResponse(request, _statefulService, _stateReader, _bufferSize)) {
      return;
    }

    // Fire update handlers SYNCHRONOUSLY after queueing the response.
    // The legacy path scheduled this via request->onDisconnect, but
    // HTTP/1.1 keep-alive (Chromium / Firefox default) holds the
    // connection open for ~60 s reuse window, so onDisconnect doesn't
    // fire until either timeout or browser tab close. Users hitting
    // Save → Reboot in quick succession lost everything because the
    // ConfigManager save call sat in the unfired onDisconnect lambda.
    // Running the handlers here means the pending-save flag (or the
    // direct flash write) lands before the user can pull the plug.
    // Services that touch flash (WiFi/AP/MQTT/NTP) defer the actual
    // littlefs commit to their main-loop iteration via _pendingSave
    // flags so the async-tcp task isn't blocked.
    if (outcome == StateUpdateResult::CHANGED) {
      _statefulService->callUpdateHandlers(HTTP_ENDPOINT_ORIGIN_ID);
    }
  }
};

template <class T>
class HttpEndpoint : public HttpGetEndpoint<T>, public HttpPostEndpoint<T> {
 public:
  HttpEndpoint(JsonStateReader<T> stateReader,
               JsonStateUpdater<T> stateUpdater,
               StatefulService<T>* statefulService,
               AsyncWebServer* server,
               const String& servicePath,
               SecurityManager* securityManager,
               AuthenticationPredicate authenticationPredicate = AuthenticationPredicates::IS_ADMIN,
               size_t bufferSize = DEFAULT_BUFFER_SIZE) :
      HttpGetEndpoint<T>(stateReader,
                         statefulService,
                         server,
                         servicePath,
                         securityManager,
                         authenticationPredicate,
                         bufferSize),
      HttpPostEndpoint<T>(stateReader,
                          stateUpdater,
                          statefulService,
                          server,
                          servicePath,
                          securityManager,
                          authenticationPredicate,
                          bufferSize) {
  }

  HttpEndpoint(JsonStateReader<T> stateReader,
               JsonStateUpdater<T> stateUpdater,
               StatefulService<T>* statefulService,
               AsyncWebServer* server,
               const String& servicePath,
               size_t bufferSize = DEFAULT_BUFFER_SIZE) :
      HttpGetEndpoint<T>(stateReader, statefulService, server, servicePath, bufferSize),
      HttpPostEndpoint<T>(stateReader, stateUpdater, statefulService, server, servicePath, bufferSize) {
  }

  // Phase 7c — expose this endpoint through WebManager's proxy
  // dispatch path so the mship-ui reverse proxy (and the Phase 7d
  // ws-bridge that funnels through it) can reach it. Without this
  // call, GET/POST hit the AsyncWebServer route locally on the LAN
  // but rest.proxy / ws-bridge see no_handler_for_path. Pass the
  // SAME servicePath the endpoint was constructed with.
  //
  // Pattern (one-liner in the owning service's constructor):
  //   _httpEndpoint(read, update, this, server, PATH, this),
  //   ...
  //   _httpEndpoint.registerProxy(web, PATH);  // <- add this
  //
  // Templated on W so `web->registerProxyEndpoint(...)` is a dependent
  // name (lookup deferred to instantiation, by which point consumers
  // have already included <WebManager.h>). Dodges the WebManager.h
  // <-> WebFeatureDelegate.h <-> HttpEndpoint.h include cycle.
  template<typename W>
  void registerProxy(W* web, const String& path) {
    if (!web) return;
    auto* svc        = HttpGetEndpoint<T>::_statefulService;
    auto  stateReader = HttpGetEndpoint<T>::_stateReader;
    auto  stateUpdater = HttpPostEndpoint<T>::_stateUpdater;

    web->registerProxyEndpoint("GET", path,
        [svc, stateReader](JsonVariant /*body*/, JsonVariant out,
                            int& out_status) {
          JsonObject obj = out.to<JsonObject>();
          svc->read(obj, stateReader);
          out_status = 200;
          return true;
        });
    web->registerProxyEndpoint("POST", path,
        [svc, stateReader, stateUpdater]
        (JsonVariant body, JsonVariant out, int& out_status) {
          if (!body.is<JsonObject>()) {
            out_status = 400;
            return true;
          }
          JsonObject in = body.as<JsonObject>();
          StateUpdateResult outcome =
              svc->updateWithoutPropagation(in, stateUpdater);
          if (outcome == StateUpdateResult::ERROR) {
            out_status = 400;
            return true;
          }
          JsonObject ret = out.to<JsonObject>();
          svc->read(ret, stateReader);
          out_status = 200;
          if (outcome == StateUpdateResult::CHANGED) {
            svc->callUpdateHandlers(HTTP_ENDPOINT_ORIGIN_ID);
          }
          return true;
        });
  }
};

#endif  // end HttpEndpoint
