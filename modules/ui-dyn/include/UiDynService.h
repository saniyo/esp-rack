#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>

class SecurityManager;
class WebManager;

#ifndef UI_DYN_SERVICE_PATH
#define UI_DYN_SERVICE_PATH "/rest/uiDynamicManifest"
#endif

#ifndef MAX_UI_DYN_SIZE
#define MAX_UI_DYN_SIZE 4096
#endif

class UiDynService {
 public:
  static constexpr uint16_t kSchemaVersion = 1;
  static constexpr size_t kMaxControls = 16;

  enum class HttpMethod : uint8_t { Get, Post, None };

  struct Endpoint {
    HttpMethod method;
    const char* path;
    bool set;

    constexpr Endpoint(HttpMethod m = HttpMethod::None, const char* p = nullptr, bool s = false) :
        method(m), path(p), set(s) {
    }
  };

  // helper-и БЕЗ constexpr return { ... } (щоб не ламалось на C++11)
  static inline Endpoint GET(const char* path) {
    return Endpoint(HttpMethod::Get, path, true);
  }
  static inline Endpoint POST(const char* path) {
    return Endpoint(HttpMethod::Post, path, true);
  }
  static inline Endpoint WS(const char* path) {
    return Endpoint(HttpMethod::None, path, true);
  }
  static inline Endpoint NONE() {
    return Endpoint(HttpMethod::None, nullptr, false);
  }

  struct ControlSpec {
    const char* id = nullptr;         // optional (auto якщо null/empty)
    const char* title = nullptr;      // required
    const char* component = nullptr;  // required

    Endpoint restRead = NONE();
    Endpoint restUpdate = NONE();
    Endpoint ws = NONE();
  };

  struct ControlInfo {
    String id, title, component;

    HttpMethod restReadMethod = HttpMethod::None;
    String restReadPath;
    bool hasRestRead = false;

    HttpMethod restUpdateMethod = HttpMethod::None;
    String restUpdatePath;
    bool hasRestUpdate = false;

    String wsPath;
    bool hasWs = false;

    bool used = false;
  };

 public:
  UiDynService(AsyncWebServer* server,
               SecurityManager* securityManager = nullptr,
               const char* path = UI_DYN_SERVICE_PATH,
               size_t jsonCapacity = MAX_UI_DYN_SIZE);

  void begin();  // монтує GET endpoint (JWT protected якщо є securityManager)

  // Phase 7c — also expose the manifest GET through WebManager's proxy
  // dispatch so the mship-ui reverse proxy can reach it (proxyDispatch
  // walks _entries + _proxyEndpoints, not the AsyncWebServer route
  // table that begin() mounts on).
  void registerProxy(WebManager* web);

  String registerControl(const ControlSpec& spec);

  const ControlInfo* find(const char* id) const;
  ControlInfo* find(const char* id);

  size_t count() const {
    return _count;
  }
  const ControlInfo& at(size_t i) const {
    return _controls[i];
  }

  const ControlInfo* findOwnerByRestPath(HttpMethod method, const char* path) const;
  const ControlInfo* findOwnerByWsPath(const char* path) const;

  void toJson(JsonObject root) const;

 private:
  ControlInfo* alloc_(const char* id);

  String makeAutoId_(const ControlSpec& spec) const;
  String slugify_(const char* s) const;
  String ensureUniqueId_(const String& base) const;

 private:
  AsyncWebServer* _server = nullptr;
  SecurityManager* _securityManager = nullptr;

  String _path;
  size_t _jsonCapacity = MAX_UI_DYN_SIZE;
  bool _mounted = false;

  ControlInfo _controls[kMaxControls];
  size_t _count = 0;
};
