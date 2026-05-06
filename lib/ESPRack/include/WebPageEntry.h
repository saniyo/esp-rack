#ifndef WebPageEntry_h
#define WebPageEntry_h

#include <utility>

#include <IWebFeatureEntry.h>
#include <WebFeatureSpec.h>

// Manifest-only entry for services that own raw AsyncWebServer handlers (e.g.
// FileSystemService with 28 endpoints). WebPageEntry records the handler map
// by `role` so the frontend can resolve URLs dynamically, but does NOT bind
// any routes — the service is still responsible for its own server->on calls.
class WebPageEntry : public IWebFeatureEntry {
 public:
  explicit WebPageEntry(WebPageSpec spec) : _spec(std::move(spec)) {}

  const char* id() const override { return _spec.id ? _spec.id : ""; }
  Kind kind() const override { return Kind::Page; }

  void registerEndpoints(AsyncWebServer* /*server*/, SecurityManager* /*sm*/) override {}

  void toJson(JsonObject& obj) const override {
    obj["id"] = _spec.id ? _spec.id : "";
    obj["kind"] = "page";
    obj["title"] = _spec.title ? _spec.title : "";
    obj["component"] = _spec.component ? _spec.component : "";
    obj["auth"] = webAuthLevelToStr(_spec.auth);

    if (_spec.routeTemplate) {
      obj["route"] = _spec.routeTemplate;
    } else if (_spec.id) {
      String route = String("/") + _spec.id + "/*";
      obj["route"] = route;
    } else {
      obj["route"] = (const char*)nullptr;
    }

    if (_spec.menu.label) {
      JsonObject menu = obj.createNestedObject("menu");
      menu["label"] = _spec.menu.label;
      if (_spec.menu.icon) menu["icon"] = _spec.menu.icon;
      menu["order"] = _spec.menu.order;
      menu["auth"] = webAuthLevelToStr(_spec.menu.auth);
      menu["hidden"] = _spec.menu.hidden;
    }

    JsonArray endpoints = obj.createNestedArray("endpoints");
    for (const auto& e : _spec.endpoints) {
      JsonObject je = endpoints.createNestedObject();
      if (e.method) je["method"] = e.method;
      if (e.path) je["path"] = e.path;
      je["auth"] = webAuthLevelToStr(e.auth);
      if (e.role) je["role"] = e.role;
    }

    JsonArray actions = obj.createNestedArray("actions");
    for (const auto& a : _spec.actions) {
      JsonObject ja = actions.createNestedObject();
      if (a.id) ja["id"] = a.id;
      if (a.title) ja["title"] = a.title;
      if (a.icon) ja["icon"] = a.icon;
      if (a.color) ja["color"] = a.color;
      if (a.restPath) ja["restPath"] = a.restPath;
      if (a.method) ja["method"] = a.method;
      ja["auth"] = webAuthLevelToStr(a.auth);
      if (a.confirm) ja["confirm"] = a.confirm;
      if (a.successMessage) ja["successMessage"] = a.successMessage;
    }
  }

 private:
  WebPageSpec _spec;
};

#endif
