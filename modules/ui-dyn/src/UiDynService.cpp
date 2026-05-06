#include "UiDynService.h"
#include <SecurityManager.h>

static const char* methodToStr_(UiDynService::HttpMethod m) {
  switch (m) {
    case UiDynService::HttpMethod::Get:
      return "GET";
    case UiDynService::HttpMethod::Post:
      return "POST";
    default:
      return "";
  }
}

static bool isAlnum_(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

UiDynService::UiDynService(AsyncWebServer* server,
                           SecurityManager* securityManager,
                           const char* path,
                           size_t jsonCapacity) :
    _server(server),
    _securityManager(securityManager),
    _path(path ? path : UI_DYN_SERVICE_PATH),
    _jsonCapacity(jsonCapacity) {
}

void UiDynService::begin() {
  if (_mounted || !_server)
    return;

  ArRequestHandlerFunction handler = [this](AsyncWebServerRequest* request) {
    auto* response = new AsyncJsonResponse(false, _jsonCapacity);
    JsonObject root = response->getRoot();
    toJson(root);
    response->setLength();
    request->send(response);
  };

  if (_securityManager) {
    _server->on(
        _path.c_str(), HTTP_GET, _securityManager->wrapRequest(handler, AuthenticationPredicates::IS_AUTHENTICATED));
  } else {
    _server->on(_path.c_str(), HTTP_GET, handler);
  }

  _mounted = true;
}

UiDynService::ControlInfo* UiDynService::alloc_(const char* id) {
  if (_count >= kMaxControls)
    return nullptr;
  auto& c = _controls[_count++];
  c.used = true;
  c.id = id;
  return &c;
}

UiDynService::ControlInfo* UiDynService::find(const char* id) {
  if (!id)
    return nullptr;
  for (size_t i = 0; i < _count; i++) {
    if (_controls[i].used && _controls[i].id == id)
      return &_controls[i];
  }
  return nullptr;
}

const UiDynService::ControlInfo* UiDynService::find(const char* id) const {
  if (!id)
    return nullptr;
  for (size_t i = 0; i < _count; i++) {
    if (_controls[i].used && _controls[i].id == id)
      return &_controls[i];
  }
  return nullptr;
}

String UiDynService::slugify_(const char* s) const {
  if (!s || !*s)
    return "control";

  String src = s;
  if (src.endsWith("Control"))
    src.remove(src.length() - 7);

  String out;
  out.reserve(32);

  bool lastDash = false;

  for (size_t i = 0; i < src.length(); i++) {
    char c = src[i];
    if (c >= 'A' && c <= 'Z')
      c = char(c - 'A' + 'a');

    if (isAlnum_(c)) {
      out += c;
      lastDash = false;
    } else {
      if (!out.isEmpty() && !lastDash) {
        out += '-';
        lastDash = true;
      }
    }
  }

  while (out.endsWith("-"))
    out.remove(out.length() - 1);
  if (out.isEmpty())
    out = "control";
  return out;
}

String UiDynService::ensureUniqueId_(const String& base) const {
  if (!find(base.c_str()))
    return base;

  for (int n = 2; n < 1000; n++) {
    String candidate = base + "-" + String(n);
    if (!find(candidate.c_str()))
      return candidate;
  }
  return "";
}

String UiDynService::makeAutoId_(const ControlSpec& spec) const {
  const char* seed = (spec.component && *spec.component) ? spec.component : spec.title;
  String base = slugify_(seed);
  return ensureUniqueId_(base);
}

String UiDynService::registerControl(const ControlSpec& spec) {
  if (!spec.title || !*spec.title)
    return "";
  if (!spec.component || !*spec.component)
    return "";

  String id;
  if (spec.id && *spec.id)
    id = spec.id;
  else
    id = makeAutoId_(spec);

  if (id.isEmpty())
    return "";

  ControlInfo* c = find(id.c_str());
  if (!c)
    c = alloc_(id.c_str());
  if (!c)
    return "";

  c->id = id;
  c->title = spec.title;
  c->component = spec.component;

  c->hasRestRead = spec.restRead.set && spec.restRead.path && spec.restRead.method != HttpMethod::None;
  if (c->hasRestRead) {
    c->restReadMethod = spec.restRead.method;
    c->restReadPath = spec.restRead.path;
  } else {
    c->restReadMethod = HttpMethod::None;
    c->restReadPath = "";
  }

  c->hasRestUpdate = spec.restUpdate.set && spec.restUpdate.path && spec.restUpdate.method != HttpMethod::None;
  if (c->hasRestUpdate) {
    c->restUpdateMethod = spec.restUpdate.method;
    c->restUpdatePath = spec.restUpdate.path;
  } else {
    c->restUpdateMethod = HttpMethod::None;
    c->restUpdatePath = "";
  }

  c->hasWs = spec.ws.set && spec.ws.path && *spec.ws.path;
  if (c->hasWs)
    c->wsPath = spec.ws.path;
  else
    c->wsPath = "";

  return id;
}

const UiDynService::ControlInfo* UiDynService::findOwnerByRestPath(HttpMethod method, const char* path) const {
  if (!path || !*path)
    return nullptr;

  for (size_t i = 0; i < _count; i++) {
    const auto& c = _controls[i];
    if (!c.used)
      continue;

    if (c.hasRestRead && c.restReadMethod == method && c.restReadPath == path)
      return &c;
    if (c.hasRestUpdate && c.restUpdateMethod == method && c.restUpdatePath == path)
      return &c;
  }
  return nullptr;
}

const UiDynService::ControlInfo* UiDynService::findOwnerByWsPath(const char* path) const {
  if (!path || !*path)
    return nullptr;

  for (size_t i = 0; i < _count; i++) {
    const auto& c = _controls[i];
    if (!c.used)
      continue;

    if (c.hasWs && c.wsPath == path)
      return &c;
  }
  return nullptr;
}

void UiDynService::toJson(JsonObject root) const {
  root["schemaVersion"] = kSchemaVersion;
  JsonArray controls = root.createNestedArray("controls");

  for (size_t i = 0; i < _count; i++) {
    const auto& c = _controls[i];
    if (!c.used)
      continue;

    JsonObject jc = controls.createNestedObject();
    jc["id"] = c.id;
    jc["title"] = c.title;
    jc["component"] = c.component;

    JsonObject endpoints = jc.createNestedObject("endpoints");

    JsonObject rest = endpoints.createNestedObject("rest");
    if (c.hasRestRead) {
      JsonObject read = rest.createNestedObject("read");
      read["method"] = methodToStr_(c.restReadMethod);
      read["path"] = c.restReadPath;
    }
    if (c.hasRestUpdate) {
      JsonObject upd = rest.createNestedObject("update");
      upd["method"] = methodToStr_(c.restUpdateMethod);
      upd["path"] = c.restUpdatePath;
    }

    if (c.hasWs) {
      JsonObject ws = endpoints.createNestedObject("ws");
      ws["path"] = c.wsPath;
    }
  }
}
