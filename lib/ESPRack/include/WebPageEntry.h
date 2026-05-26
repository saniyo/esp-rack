#ifndef WebPageEntry_h
#define WebPageEntry_h

#include <utility>

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>

#include <IWebFeatureEntry.h>
#include <WebFeatureSpec.h>

// Owns a set of endpoints declared via a WebPageSpec. Each endpoint
// in spec.endpoints[] may carry its own handler / jsonHandler /
// internalHandler — registerEndpoints binds the appropriate one
// against AsyncWebServer for browser-driven requests, and
// proxyDispatch routes in-process calls from Mothership's
// rest.proxy directly into internalHandler.
//
// Migration path away from `server->on(...)` scattered in service
// constructors: a service builds its WebPageSpec with handlers
// populated, hands it to web->registerPage(spec), and WebPageEntry
// owns the binding lifecycle. The service itself stays out of the
// raw AsyncWebServer API.
class WebPageEntry : public IWebFeatureEntry {
 public:
  explicit WebPageEntry(WebPageSpec spec) : _spec(std::move(spec)) {}

  const char* id() const override { return _spec.id ? _spec.id : ""; }
  Kind kind() const override { return Kind::Page; }

  void registerEndpoints(AsyncWebServer* server, SecurityManager* sm) override {
    if (_mounted || !server) return;

    for (const auto& e : _spec.endpoints) {
      if (!e.path) continue;
      const char* method = e.method ? e.method : "GET";
      auto predicate = webAuthLevelToPredicate(e.auth);

      if (e.jsonHandler) {
        // POST/PUT/PATCH path that wants its body auto-parsed.
        // AsyncCallbackJsonWebHandler buffers + parses + calls back.
        // It accepts any method via setMethod() on the handler.
        auto wrapped = sm
            ? sm->wrapCallback(e.jsonHandler, predicate)
            : e.jsonHandler;
        auto* h = new AsyncCallbackJsonWebHandler(e.path, wrapped);
        if (strcmp(method, "POST") == 0)         h->setMethod(HTTP_POST);
        else if (strcmp(method, "PUT") == 0)     h->setMethod(HTTP_PUT);
        else if (strcmp(method, "PATCH") == 0)   h->setMethod(HTTP_PATCH);
        else if (strcmp(method, "DELETE") == 0)  h->setMethod(HTTP_DELETE);
        else                                      h->setMethod(HTTP_POST);
        server->addHandler(h);
        continue;
      }

      if (e.handler) {
        auto wrapped = sm
            ? sm->wrapRequest(e.handler, predicate)
            : e.handler;
        if (strcmp(method, "GET") == 0)          server->on(e.path, HTTP_GET, wrapped);
        else if (strcmp(method, "POST") == 0)    server->on(e.path, HTTP_POST, wrapped);
        else if (strcmp(method, "PUT") == 0)     server->on(e.path, HTTP_PUT, wrapped);
        else if (strcmp(method, "DELETE") == 0)  server->on(e.path, HTTP_DELETE, wrapped);
        else                                      server->on(e.path, HTTP_GET, wrapped);
        continue;
      }

      // No handler nor jsonHandler — endpoint is bind-deferred (e.g.
      // multipart upload that the service still registers manually,
      // but wants to surface in the manifest + reach via rest.proxy
      // through internalHandler).
    }

    // Auxiliary spec.actions[] (legacy / cross-cut button-style
    // entries belonging to the same page) — bind with the same
    // handler-only path; jsonHandler-on-action is rare but
    // structurally supported through the WebActionEntry already.
    // We don't replicate WebActionEntry's logic here; if a service
    // needs an action, it should call web->registerAction(spec).

    _mounted = true;
  }

  void toJson(JsonObject& obj) const override {
    obj["id"] = _spec.id ? _spec.id : "";
    obj["kind"] = "page";
    obj["title"] = _spec.title ? _spec.title : "";
    if (_spec.version) obj["version"] = _spec.version;
    obj["component"] = _spec.component ? _spec.component : "";
    obj["auth"] = webAuthLevelToStr(_spec.auth);

    if (_spec.routeTemplate) {
      obj["route"] = _spec.routeTemplate;
    } else if (_spec.id) {
      // See WebInfoEntry::toJson — stack buffer instead of transient
      // Arduino String. ArduinoJson copies into its own pool.
      char route[96];
      snprintf(route, sizeof(route), "/%s/*", _spec.id);
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

  // Phase 2 — rest.proxy entry point. Mothership walks every
  // IWebFeatureEntry; WebPageEntry matches against each endpoint's
  // (method, path) pair and invokes its internalHandler. Refuses
  // (returns 501) when the endpoint is in the spec but no
  // internalHandler is set — that's a deliberate opt-out on the
  // service's part (e.g. multipart upload stays raw).
  bool proxyDispatch(const char* method,
                      const char* path,
                      JsonVariant body,
                      int& out_status,
                      JsonVariant out_body) override {
    if (!method || !path) return false;
    for (const auto& e : _spec.endpoints) {
      if (!e.path) continue;
      if (strcmp(e.path, path) != 0) continue;
      const char* want_method = e.method ? e.method : "GET";
      if (strcmp(want_method, method) != 0) {
        // Path matches but method doesn't — keep walking the list
        // (a service might register the same path under two methods).
        continue;
      }
      if (!e.internalHandler) {
        out_status = 501;
        JsonObject o = out_body.to<JsonObject>();
        o["error"]   = "no_internal_handler";
        o["message"] = "endpoint registered but doesn't expose an "
                       "in-process handler — invoke via direct HTTP";
        return true;
      }
      out_status = 200;
      e.internalHandler(body, out_body, out_status);
      return true;
    }
    return false;
  }

 private:
  WebPageSpec _spec;
  bool _mounted{false};
};

#endif
