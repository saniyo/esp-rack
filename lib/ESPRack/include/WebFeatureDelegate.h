#ifndef WebFeatureDelegate_h
#define WebFeatureDelegate_h

#include <cstring>
#include <memory>
#include <utility>

#include <HttpEndpoint.h>
#include <WsManager.h>

#include <IWebFeatureEntry.h>
#include <WebFeatureSpec.h>

// Typed aggregator for a single StatefulService<T>: REST (GET/POST) + optional
// WS + manifest metadata. The service stores a raw pointer to the entry (via
// WebManager::registerFeature) and uses broadcastWs() to push state over WS.
// WebManager owns the lifetime through std::unique_ptr<IWebFeatureEntry>.
template <typename T>
class WebFeatureEntry : public IWebFeatureEntry {
 public:
  WebFeatureEntry(WebFeatureSpec spec,
                  StatefulService<T>* service,
                  JsonStateReader<T> restReader,
                  JsonStateUpdater<T> restUpdater,
                  JsonStateReader<T> wsReader,
                  JsonStateUpdater<T> wsUpdater,
                  WsManager* wsManager,
                  size_t restBufferSize = DEFAULT_BUFFER_SIZE,
                  size_t wsBufferSize = DEFAULT_BUFFER_SIZE)
      : _spec(std::move(spec)),
        _service(service),
        _restReader(restReader),
        _restUpdater(restUpdater),
        _wsReader(wsReader),
        _wsUpdater(wsUpdater),
        _wsManager(wsManager),
        _restBufferSize(restBufferSize),
        _wsBufferSize(wsBufferSize) {}

  WebFeatureEntry(WebFeatureSpec spec,
                  StatefulService<T>* service,
                  JsonStateReader<T> reader,
                  JsonStateUpdater<T> updater,
                  WsManager* wsManager,
                  size_t bufferSize = DEFAULT_BUFFER_SIZE)
      : WebFeatureEntry(std::move(spec), service, reader, updater, reader, updater, wsManager, bufferSize, bufferSize) {}

  const char* id() const override { return _spec.id ? _spec.id : ""; }
  Kind kind() const override { return Kind::Feature; }

  void registerEndpoints(AsyncWebServer* server, SecurityManager* sm) override {
    if (_mounted || !server || !_service) return;

    auto predicate = webAuthLevelToPredicate(_spec.auth);

    if (_spec.restRead && _spec.restUpdate && std::strcmp(_spec.restRead, _spec.restUpdate) == 0) {
      _endpoint.reset(new HttpEndpoint<T>(_restReader, _restUpdater, _service, server,
                                          _spec.restRead, sm, predicate, _restBufferSize));
    } else {
      if (_spec.restRead) {
        _getEndpoint.reset(new HttpGetEndpoint<T>(_restReader, _service, server,
                                                  _spec.restRead, sm, predicate, _restBufferSize));
      }
      if (_spec.restUpdate) {
        _postEndpoint.reset(new HttpPostEndpoint<T>(_restReader, _restUpdater, _service, server,
                                                    _spec.restUpdate, sm, predicate, _restBufferSize));
      }
    }

    // Heap-allocate the Binding so its address never changes. Its ctor registers
    // an update handler that captures `this` — if we stored by value and moved,
    // that lambda would dangle.
    if (_spec.wsPath && _wsManager && !_wsBinding) {
      _wsBinding.reset(new WsManager::Binding<T>(_wsManager, _spec.wsPath, _wsBufferSize,
                                                 _service, _wsReader, _wsUpdater, true));
    }

    _mounted = true;
  }

  void toJson(JsonObject& obj) const override {
    obj["id"] = _spec.id ? _spec.id : "";
    obj["kind"] = "feature";
    obj["title"] = _spec.title ? _spec.title : "";
    if (_spec.version) obj["version"] = _spec.version;
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

    if (_spec.restRead || _spec.restUpdate) {
      JsonObject rest = obj.createNestedObject("rest");
      if (_spec.restRead) rest["read"] = _spec.restRead;
      if (_spec.restUpdate) rest["update"] = _spec.restUpdate;
    }
    if (_spec.wsPath) obj["ws"] = _spec.wsPath;

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

  // Push current state over WS (typed broadcast helper for the owning service).
  void broadcastWs(const String& origin = "") {
    if (_wsBinding) _wsBinding->broadcastCurrentState(origin);
  }

  // Phase 2 — in-process rest.proxy entry point. Matches the
  // request's (method, path) pair against this feature's
  // restRead / restUpdate. On match: GET re-uses _restReader to
  // emit the form JSON; POST runs _restUpdater under the service's
  // transaction guard and then re-reads to surface the new state
  // back to the caller. Auth bypassed — caller is mothership-action
  // which already cleared the mTLS check-in gate.
  bool proxyDispatch(const char* method,
                      const char* path,
                      JsonVariant body,
                      int& out_status,
                      JsonVariant out_body) override {
    if (!path || !_service) return false;

    const bool is_get  = method && String(method) == "GET";
    const bool is_post = method && (String(method) == "POST"
                                      || String(method) == "PUT");

    // Match against the feature's primary read/update endpoints.
    const bool match_read   = _spec.restRead   && String(_spec.restRead)   == path;
    const bool match_update = _spec.restUpdate && String(_spec.restUpdate) == path;

    if (!match_read && !match_update) return false;

    // GET path — read state into out_body.
    if (is_get) {
      out_status = 200;
      JsonObject root = out_body.to<JsonObject>();
      _service->read(root, _restReader);
      return true;
    }

    // POST / PUT path — drive an update through the service's
    // transactional API, then surface the resulting state.
    if (is_post && match_update) {
      JsonObject in_obj = body.as<JsonObject>();
      StateUpdateResult outcome =
          _service->update(in_obj, _restUpdater, "rest.proxy");
      if (outcome == StateUpdateResult::ERROR) {
        out_status = 400;
        JsonObject err = out_body.to<JsonObject>();
        err["error"] = "update_rejected";
        return true;
      }
      out_status = 200;
      JsonObject root = out_body.to<JsonObject>();
      _service->read(root, _restReader);
      return true;
    }

    // Method/path combo didn't match a known pair — return 405 so
    // operator's UI surfaces something better than a generic 404.
    out_status = 405;
    JsonObject err = out_body.to<JsonObject>();
    err["error"] = "method_not_allowed";
    return true;
  }

  // True if at least one WS client is currently subscribed to this feature's
  // endpoint. Services call this BEFORE doing any periodic/event work
  // dedicated to WS — e.g. NTP's 1 Hz clock tick — so the main loop skips
  // timestamp arithmetic, state reads and allocations when nobody's watching.
  bool hasSubscribers() const {
    if (!_wsManager || !_spec.wsPath || !*_spec.wsPath) return false;
    return _wsManager->hasClients(_spec.wsPath);
  }

  StatefulService<T>* service() const { return _service; }
  const WebFeatureSpec& spec() const { return _spec; }

 private:
  WebFeatureSpec _spec;
  StatefulService<T>* _service{nullptr};
  JsonStateReader<T> _restReader;
  JsonStateUpdater<T> _restUpdater;
  JsonStateReader<T> _wsReader;
  JsonStateUpdater<T> _wsUpdater;
  WsManager* _wsManager{nullptr};
  size_t _restBufferSize{DEFAULT_BUFFER_SIZE};
  size_t _wsBufferSize{DEFAULT_BUFFER_SIZE};

  std::unique_ptr<HttpEndpoint<T>> _endpoint;
  std::unique_ptr<HttpGetEndpoint<T>> _getEndpoint;
  std::unique_ptr<HttpPostEndpoint<T>> _postEndpoint;
  std::unique_ptr<WsManager::Binding<T>> _wsBinding;

  bool _mounted{false};
};

#endif
