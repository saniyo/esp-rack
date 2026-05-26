#ifndef WebActionEntry_h
#define WebActionEntry_h

#include <utility>

#include <IWebFeatureEntry.h>
#include <WebFeatureSpec.h>

// Top-level action — one-shot HTTP endpoint + manifest metadata.
// Forms embed it via addActionField(…, actionRef("id")) and resolve
// URL/method/confirm/color/icon from the manifest entry; standalone
// renderers (page bars, menu bars) can also iterate manifest.features
// and pick kind=="action" entries.
class WebActionEntry : public IWebFeatureEntry {
 public:
  explicit WebActionEntry(WebActionSpec spec) : _spec(std::move(spec)) {}

  const char* id() const override { return _spec.id ? _spec.id : ""; }
  Kind kind() const override { return Kind::Action; }

  void registerEndpoints(AsyncWebServer* server, SecurityManager* sm) override {
    if (_mounted || !server || !_spec.id) return;

    char pathBuf[80];
    fillResolvedPath(pathBuf, sizeof(pathBuf));
    const char* path = pathBuf;

    auto predicate = webAuthLevelToPredicate(_spec.auth);
    auto handler = _spec.handler
        ? _spec.handler
        : std::function<void(AsyncWebServerRequest*)>([](AsyncWebServerRequest* r) { r->send(200); });

    ArRequestHandlerFunction wrapped = sm
        ? sm->wrapRequest(handler, predicate)
        : handler;

    const char* method = _spec.method ? _spec.method : "POST";
    if (strcmp(method, "GET") == 0) {
      server->on(path, HTTP_GET, wrapped);
    } else if (strcmp(method, "DELETE") == 0) {
      server->on(path, HTTP_DELETE, wrapped);
    } else if (strcmp(method, "PUT") == 0) {
      server->on(path, HTTP_PUT, wrapped);
    } else {
      server->on(path, HTTP_POST, wrapped);
    }

    _mounted = true;
  }

  void toJson(JsonObject& obj) const override {
    obj["id"] = _spec.id ? _spec.id : "";
    obj["kind"] = "action";
    if (_spec.title) obj["title"] = _spec.title;
    // Always materialise the path into pathBuf and assign as an array.
    // ArduinoJson's `const char*` overload uses a Link storage policy
    // (no copy, holds the pointer) which would dangle once we return
    // — pathBuf is local. The `char[N]` overload uses Copy.
    char pathBuf[80];
    fillResolvedPath(pathBuf, sizeof(pathBuf));
    obj["restPath"] = pathBuf;
    obj["method"] = _spec.method ? _spec.method : "POST";
    obj["auth"] = webAuthLevelToStr(_spec.auth);
    if (_spec.icon) obj["icon"] = _spec.icon;
    if (_spec.color) obj["color"] = _spec.color;
    if (_spec.confirm) obj["confirm"] = _spec.confirm;
    if (_spec.successMessage) obj["successMessage"] = _spec.successMessage;
  }

  // Phase 2 — in-process rest.proxy entry point. Mothership's
  // actionRestProxy walks every IWebFeatureEntry; this one says yes
  // only when (method, path) matches AND the spec carried an
  // internalHandler. The AsyncWebServerRequest-based `handler`
  // stays for browser-driven local-UI calls.
  bool proxyDispatch(const char* method,
                      const char* path,
                      JsonVariant body,
                      int& out_status,
                      JsonVariant out_body) override {
    if (!_spec.id || !path) return false;
    char pathBuf[80];
    fillResolvedPath(pathBuf, sizeof(pathBuf));
    if (strcmp(pathBuf, path) != 0) return false;
    const char* want_method = _spec.method ? _spec.method : "POST";
    if (method && strcmp(want_method, method) != 0) {
      out_status = 405;
      JsonObject o = out_body.to<JsonObject>();
      o["error"] = "method_not_allowed";
      return true;
    }
    if (!_spec.internalHandler) {
      out_status = 501;
      JsonObject o = out_body.to<JsonObject>();
      o["error"] = "action_has_no_internal_handler";
      o["message"] = "module must populate WebActionSpec::internalHandler "
                     "for the action to be reachable via rest.proxy";
      return true;
    }
    out_status = 200;
    _spec.internalHandler(body, out_body, out_status);
    return true;
  }

  const WebActionSpec& spec() const { return _spec; }

 private:
  // Fill `buf` with the resolved REST path. Always writes into the
  // caller-supplied buffer — when `_spec.restPath` is set we copy it
  // in too, so the caller can rely on the buffer holding the answer
  // (this matters for ArduinoJson assignment: the `char[N]` overload
  // copies; the `const char*` overload would Link-store the pointer,
  // which is dangling if it pointed at this local buffer).
  // Replaces the previous String-returning version that allocated a
  // transient Arduino String on every toJson / proxyDispatch call.
  void fillResolvedPath(char* buf, size_t buf_sz) const {
    if (buf_sz == 0) return;
    if (_spec.restPath && _spec.restPath[0] != '\0') {
      strncpy(buf, _spec.restPath, buf_sz - 1);
      buf[buf_sz - 1] = '\0';
      return;
    }
    snprintf(buf, buf_sz, "/rest/action/%s",
             _spec.id ? _spec.id : "unknown");
  }

  WebActionSpec _spec;
  bool _mounted{false};
};

#endif
