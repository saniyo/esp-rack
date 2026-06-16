#ifndef HttpEndpoint_h
#define HttpEndpoint_h

#include <functional>

#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>

#include <AdaptiveJson.h>
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

// ---- Typed-feature state response --------------------------------------
// Thin wrapper over sendAdaptiveJson (AdaptiveJson.h): the configured
// buffer is just the measured starting point — the doc grows on overflow,
// shrinks under heap pressure, streams wire-chunked, and 503s only when
// the heap genuinely can't serve. See AdaptiveJson.h for the full policy.
// Returns true if a 2xx response was queued, false after the 503.
template <class T>
inline bool sendJsonStateResponse(AsyncWebServerRequest* request,
                                  StatefulService<T>* service,
                                  const JsonStateReader<T>& reader,
                                  size_t configuredSize) {
  return sendAdaptiveJson(
      request,
      [service, reader](JsonObject& root) { service->read(root, reader); },
      configuredSize ? configuredSize : DEFAULT_BUFFER_SIZE);
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
