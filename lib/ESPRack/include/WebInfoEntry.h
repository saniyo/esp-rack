#ifndef WebInfoEntry_h
#define WebInfoEntry_h

#include <algorithm>
#include <functional>
#include <utility>

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>

#include <IWebFeatureEntry.h>
#include <StatefulService.h>
#include <WebFeatureSpec.h>

// Stateless feature-metadata entry for the manifest. Two modes:
//   1) With a reader lambda: mounts a single GET at spec.restRead whose body
//      the reader writes directly into a JsonObject.
//   2) Without a reader (pure metadata): no endpoint is mounted — used when
//      the feature is a compound page whose tabs point at restPaths owned by
//      other services (e.g. the 'system' feature whose status/ota/upload
//      tabs are served by SystemStatus / OTASettings / UploadFirmware).
class WebInfoEntry : public IWebFeatureEntry {
 public:
  // Metadata-only ctor — nothing to mount, just manifest data.
  explicit WebInfoEntry(WebFeatureSpec spec)
      : _spec(std::move(spec)), _reader(), _bufferSize(0) {}

  // Reader + endpoint ctor.
  WebInfoEntry(WebFeatureSpec spec,
               std::function<void(JsonObject&)> reader,
               size_t bufferSize = DEFAULT_BUFFER_SIZE)
      : _spec(std::move(spec)), _reader(std::move(reader)), _bufferSize(bufferSize) {}

  const char* id() const override { return _spec.id ? _spec.id : ""; }
  Kind kind() const override { return Kind::Feature; }

  void registerEndpoints(AsyncWebServer* server, SecurityManager* sm) override {
    if (_mounted || !server || !_reader || !_spec.restRead) return;

    auto predicate = webAuthLevelToPredicate(_spec.auth);
    auto reader = _reader;
    size_t bufSz = _bufferSize;

    ArRequestHandlerFunction handler = [reader, bufSz](AsyncWebServerRequest* req) {
      AsyncJsonResponse* resp = new AsyncJsonResponse(false, bufSz);
      JsonObject root = resp->getRoot();
      reader(root);
      resp->setLength();
      req->send(resp);
    };

    ArRequestHandlerFunction wrapped = sm ? sm->wrapRequest(handler, predicate) : handler;
    server->on(_spec.restRead, HTTP_GET, wrapped);
    _mounted = true;
  }

  void toJson(JsonObject& obj) const override {
    obj["id"] = _spec.id ? _spec.id : "";
    obj["kind"] = "feature";
    obj["title"] = _spec.title ? _spec.title : "";
    obj["component"] = _spec.component ? _spec.component : "DynamicSettings";
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

    if (_spec.restRead) {
      JsonObject rest = obj.createNestedObject("rest");
      rest["read"] = _spec.restRead;
    }

    JsonArray tabs = obj.createNestedArray("tabs");
    for (const auto& t : _spec.tabs) {
      JsonObject jt = tabs.createNestedObject();
      if (t.key) jt["key"] = t.key;
      if (t.title) jt["title"] = t.title;
      if (t.restPath) jt["restPath"] = t.restPath;
      jt["postable"] = t.postable;
      jt["live"] = t.live;
      jt["auth"] = webAuthLevelToStr(t.auth);
    }
  }

  // Contribute a tab from another service. Enables the "compound feature"
  // pattern: App ctor registers an empty 'system' shell, then each module
  // (SystemStatus / OTA / UploadFirmware / …) contributes its own tab via
  // WebManager::addTabToFeature("system", tab) in onInstall.
  //
  // Insertion is sorted by tab.order so the System page tab order is
  // deterministic regardless of the topological install order Builder
  // chooses — without this, raising a module's priority for ANY reason
  // (e.g. adding a `requires_` edge) would silently shuffle the System
  // tab strip. Equal `order` values keep stable insertion order.
  bool addTab(const WebTabSpec& tab) override {
    auto pos = std::upper_bound(
        _spec.tabs.begin(), _spec.tabs.end(), tab,
        [](const WebTabSpec& a, const WebTabSpec& b) { return a.order < b.order; });
    _spec.tabs.insert(pos, tab);
    return true;
  }

 private:
  WebFeatureSpec _spec;
  std::function<void(JsonObject&)> _reader;
  size_t _bufferSize;
  bool _mounted{false};
};

#endif
