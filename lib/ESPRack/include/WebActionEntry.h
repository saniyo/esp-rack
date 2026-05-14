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

    const String path = resolvedPath();

    auto predicate = webAuthLevelToPredicate(_spec.auth);
    auto handler = _spec.handler
        ? _spec.handler
        : std::function<void(AsyncWebServerRequest*)>([](AsyncWebServerRequest* r) { r->send(200); });

    ArRequestHandlerFunction wrapped = sm
        ? sm->wrapRequest(handler, predicate)
        : handler;

    const String method = _spec.method ? String(_spec.method) : String("POST");
    if (method == "GET") {
      server->on(path.c_str(), HTTP_GET, wrapped);
    } else if (method == "DELETE") {
      server->on(path.c_str(), HTTP_DELETE, wrapped);
    } else if (method == "PUT") {
      server->on(path.c_str(), HTTP_PUT, wrapped);
    } else {
      server->on(path.c_str(), HTTP_POST, wrapped);
    }

    _mounted = true;
  }

  void toJson(JsonObject& obj) const override {
    obj["id"] = _spec.id ? _spec.id : "";
    obj["kind"] = "action";
    if (_spec.title) obj["title"] = _spec.title;
    obj["restPath"] = resolvedPath();
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
    if (resolvedPath() != String(path)) return false;
    const char* want_method = _spec.method ? _spec.method : "POST";
    if (method && String(want_method) != method) {
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
  String resolvedPath() const {
    if (_spec.restPath && _spec.restPath[0] != '\0') return String(_spec.restPath);
    String p = "/rest/action/";
    p += _spec.id ? _spec.id : "unknown";
    return p;
  }

  WebActionSpec _spec;
  bool _mounted{false};
};

#endif
