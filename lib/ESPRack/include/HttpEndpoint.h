#ifndef HttpEndpoint_h
#define HttpEndpoint_h

#include <functional>

#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>

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
    AsyncJsonResponse* response = new AsyncJsonResponse(false, _bufferSize);
    JsonObject jsonObject = response->getRoot().to<JsonObject>();
    _statefulService->read(jsonObject, _stateReader);

    response->setLength();
    request->send(response);
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
    AsyncJsonResponse* response = new AsyncJsonResponse(false, _bufferSize);
    jsonObject = response->getRoot().to<JsonObject>();
    _statefulService->read(jsonObject, _stateReader);
    response->setLength();
    request->send(response);

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
