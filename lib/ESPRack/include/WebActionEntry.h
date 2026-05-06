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
