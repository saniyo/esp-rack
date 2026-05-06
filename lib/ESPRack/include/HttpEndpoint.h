#ifndef HttpEndpoint_h
#define HttpEndpoint_h

#include <functional>

#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>

#include <SecurityManager.h>
#include <StatefulService.h>

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
};

#endif  // end HttpEndpoint
