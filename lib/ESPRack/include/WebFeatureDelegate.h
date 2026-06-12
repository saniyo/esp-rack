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
        _wsBufferSize(wsBufferSize),
        // This ctor took a SEPARATE ws/flat reader: it backs the machine
        // /state GET (C1). std::function isn't comparable, so we record the
        // distinction with a flag instead of comparing _wsReader==_restReader.
        _hasDistinctStateReader(true) {}

  WebFeatureEntry(WebFeatureSpec spec,
                  StatefulService<T>* service,
                  JsonStateReader<T> reader,
                  JsonStateUpdater<T> updater,
                  WsManager* wsManager,
                  size_t bufferSize = DEFAULT_BUFFER_SIZE)
      : WebFeatureEntry(std::move(spec), service, reader, updater, reader, updater, wsManager, bufferSize, bufferSize) {
    // Single reader serves both faces: the /state GET falls back to the form
    // reader (interim — documented until the module gains a distinct flat
    // reader). Undo the true the delegating ctor set above.
    _hasDistinctStateReader = false;
  }

  const char* id() const override { return _spec.id ? _spec.id : ""; }
  Kind kind() const override { return Kind::Feature; }

  void registerEndpoints(AsyncWebServer* server, SecurityManager* sm) override {
    if (_mounted || !server || !_service) return;

    // Auto-size the REST buffer from the real schema tree if the feature
    // opted in (WEBFEATURE_BUFFER_AUTO). Once, here at boot (heap fresh) —
    // never per request. The probe always reads the FORM tree; the state
    // endpoint inherits the same buffer size (C1).
    if (_restBufferSize == WEBFEATURE_BUFFER_AUTO) {
      _restBufferSize = measureRestBuffer();
    }

    // Derive the C1 mount paths from the feature id — modules no longer set
    // restRead/restUpdate (those fields are now ignored, see WebFeatureSpec).
    //   form  GET      -> /api/v1/<id>/form   (Form-DSL envelope, UI face)
    //   state GET+POST -> /api/v1/<id>/state  (flat machine values)
    // Stored as members so the HttpEndpoint ctors (const String&) reference
    // stable storage for the entry's lifetime.
    derivePaths();

    auto predicate = webAuthLevelToPredicate(_spec.auth);

    if (!_formPath.isEmpty()) {
      // FORM face — GET only, served by the form reader (the restReader the
      // module passed today). The UI saves through the state POST below, so
      // the form endpoint never needs an updater.
      _getEndpoint.reset(new HttpGetEndpoint<T>(_restReader, _service, server,
                                                _formPath, sm, predicate, _restBufferSize));
    }

    if (!_statePath.isEmpty()) {
      // STATE face — GET+POST mounted separately so the write side can
      // carry the C3 operator floor. GET uses the DISTINCT flat reader
      // when the feature was registered with a separate ws/flat reader;
      // otherwise it falls back to the form reader (interim, until the
      // module grows one). POST uses the existing updater (already
      // consumes flat dicts and {settings:{...}} envelopes) and ALSO
      // requires a write-capable role: "reader" machine tokens may GET
      // everything their auth level allows but never mutate (403).
      const JsonStateReader<T>& stateReader =
          _hasDistinctStateReader ? _wsReader : _restReader;
      auto writePredicate = [predicate](Authentication& a) {
        return predicate(a) && AuthenticationPredicates::IS_OPERATOR(a);
      };
      _stateGet.reset(new HttpGetEndpoint<T>(stateReader, _service, server,
                                             _statePath, sm, predicate, _restBufferSize));
      _statePost.reset(new HttpPostEndpoint<T>(stateReader, _restUpdater, _service, server,
                                               _statePath, sm, writePredicate, _restBufferSize));
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
      if (_spec.menu.parent) menu["parent"] = _spec.menu.parent;
      if (_spec.menu.expandTabs) menu["expandTabs"] = true;
      if (_spec.menu.role) menu["role"] = true;
    }

    // C1: paths are DERIVED from the id, not read from spec.restRead/Update.
    // rest.read   = form path  (UI face)
    // rest.update = state path (machine POST target)
    // rest.state  = state path (NEW — machine GET/POST of flat values)
    // toJson can run before registerEndpoints (manifest pre-bake at boot),
    // so derive on demand into locals rather than relying on _formPath being
    // populated yet.
    if (_spec.id && *_spec.id) {
      char formBuf[112];
      char stateBuf[112];
      snprintf(formBuf, sizeof(formBuf), "%s/%s/form", API_V1_PREFIX, _spec.id);
      snprintf(stateBuf, sizeof(stateBuf), "%s/%s/state", API_V1_PREFIX, _spec.id);
      JsonObject rest = obj.createNestedObject("rest");
      rest["read"]   = formBuf;
      rest["update"] = stateBuf;
      rest["state"]  = stateBuf;
    }
    if (_spec.wsPath) obj["ws"] = _spec.wsPath;

    JsonArray tabs = obj.createNestedArray("tabs");
    for (const auto& t : _spec.tabs) {
      JsonObject jt = tabs.createNestedObject();
      if (t.key) jt["key"] = t.key;
      if (t.title) jt["title"] = t.title;
      if (t.icon) jt["icon"] = t.icon;
      // C1 precedence: formOf (another typed feature's derived form) →
      // explicit restPath (bespoke page) → this feature's own derived
      // form path, so modules never hand-write derivable tab paths.
      if (t.formOf && *t.formOf) {
        char formBuf[112];
        snprintf(formBuf, sizeof(formBuf), "%s/%s/form", API_V1_PREFIX, t.formOf);
        jt["restPath"] = formBuf;
      } else if (t.restPath) {
        jt["restPath"] = t.restPath;
      } else if (_spec.id && *_spec.id) {
        char formBuf[112];
        snprintf(formBuf, sizeof(formBuf), "%s/%s/form", API_V1_PREFIX, _spec.id);
        jt["restPath"] = formBuf;
      }
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
      // C1: emit the DERIVED action path, never spec.restPath.
      if (a.id) {
        char actBuf[112];
        fillActionPath(actBuf, sizeof(actBuf), a.id);
        ja["restPath"] = actBuf;
      }
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

  // Phase 2 — in-process rest.proxy entry point. Matches the request's
  // (method, path) pair against this feature's DERIVED C1 paths:
  //   GET  /api/v1/<id>/form   -> form reader (UI envelope)
  //   GET  /api/v1/<id>/state  -> flat state reader (machine values)
  //   POST /api/v1/<id>/state  -> updater, then echo via the state reader
  // Auth bypassed — caller is mothership-action which already cleared the
  // mTLS check-in gate.
  bool proxyDispatch(const char* method,
                      const char* path,
                      JsonVariant body,
                      int& out_status,
                      JsonVariant out_body) override {
    if (!path || !_service || !_spec.id || !*_spec.id) return false;

    char formBuf[112];
    char stateBuf[112];
    snprintf(formBuf, sizeof(formBuf), "%s/%s/form", API_V1_PREFIX, _spec.id);
    snprintf(stateBuf, sizeof(stateBuf), "%s/%s/state", API_V1_PREFIX, _spec.id);

    const bool is_get  = method && strcmp(method, "GET") == 0;
    const bool is_post = method && (strcmp(method, "POST") == 0
                                      || strcmp(method, "PUT") == 0);

    const bool match_form  = strcmp(formBuf, path) == 0;
    const bool match_state = strcmp(stateBuf, path) == 0;

    if (!match_form && !match_state) return false;

    // The flat machine reader backs /state; the form reader backs /form.
    const JsonStateReader<T>& stateReader =
        _hasDistinctStateReader ? _wsReader : _restReader;

    // GET path — read into out_body with the face-appropriate reader.
    if (is_get) {
      out_status = 200;
      JsonObject root = out_body.to<JsonObject>();
      _service->read(root, match_form ? _restReader : stateReader);
      return true;
    }

    // POST / PUT path — only /state accepts writes. Drive an update through
    // the service's transactional API, then surface the resulting state.
    if (is_post && match_state) {
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
      _service->read(root, stateReader);
      return true;
    }

    // Method/path combo didn't match a known pair — return 405 so the
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
  // Build the REST tree once into a generous temp doc and size the runtime
  // buffer to the ACTUAL pool used + 50% headroom for variable scalar values
  // — replaces hand-guessed worst-case constants. Called once at boot (heap
  // fresh); the temp doc is freed on return. Safe fallback if the probe
  // can't allocate or the feature has no reader.
  size_t measureRestBuffer() {
    static constexpr size_t kProbe    = 24576;  // > any realistic schema
    static constexpr size_t kFallback = 8192;
    if (!_service || !_restReader) return kFallback;
    DynamicJsonDocument probe(kProbe);
    if (probe.capacity() == 0) return kFallback;            // boot heap too tight
    JsonObject root = probe.to<JsonObject>();
    if (root.isNull()) return kFallback;
    _service->read(root, _restReader);
    const size_t used = probe.memoryUsage();
    if (used == 0) return kFallback;
    // Small +512 B headroom for scalar growth beyond the default-state
    // values measured here. Kept SMALL on purpose: measured real schemas
    // (the LightState test-bench) are already ~8 KB, and the no-PSRAM C3's
    // steady maxAlloc is only ~9-10 KB — there is barely room above the
    // tree. A fat margin would push the buffer past maxAlloc and the
    // proactive guard would 503 most of the time. A schema that genuinely
    // needs more headroom than this is the signal to move it to the
    // section-stream path (Phase 3), not a bigger single-shot buffer.
    size_t sized = used + 512;
    if (probe.overflowed()) sized = kProbe;  // schema > 24K probe — needs streaming (Phase 3)
    log_d("[webfeature:%s] REST buffer auto-sized: tree=%uB -> alloc=%uB",
          _spec.id ? _spec.id : "?", (unsigned)used, (unsigned)sized);
    return sized;
  }

  // Compose the C1 form/state paths from the feature id into the String
  // members the HttpEndpoint ctors reference. No id -> both stay empty and
  // nothing mounts (a malconfigured feature simply contributes no routes).
  void derivePaths() {
    if (!_spec.id || !*_spec.id) return;
    _formPath  = String(API_V1_PREFIX) + "/" + _spec.id + "/form";
    _statePath = String(API_V1_PREFIX) + "/" + _spec.id + "/state";
  }

  WebFeatureSpec _spec;
  StatefulService<T>* _service{nullptr};
  JsonStateReader<T> _restReader;
  JsonStateUpdater<T> _restUpdater;
  JsonStateReader<T> _wsReader;
  JsonStateUpdater<T> _wsUpdater;
  WsManager* _wsManager{nullptr};
  size_t _restBufferSize{DEFAULT_BUFFER_SIZE};
  size_t _wsBufferSize{DEFAULT_BUFFER_SIZE};
  // True when the feature was registered with a SEPARATE ws/flat reader —
  // that reader backs the machine /state GET. False = single reader, /state
  // falls back to the form reader (interim). std::function isn't comparable,
  // so this is set by the ctors rather than derived at use sites.
  bool _hasDistinctStateReader{false};

  // Derived C1 mount paths, populated in registerEndpoints (derivePaths).
  // Stored so the HttpEndpoint ctors' const String& outlives the call.
  String _formPath;
  String _statePath;

  std::unique_ptr<HttpGetEndpoint<T>> _getEndpoint;   // form GET
  std::unique_ptr<HttpGetEndpoint<T>> _stateGet;      // state GET
  std::unique_ptr<HttpPostEndpoint<T>> _statePost;    // state POST (operator floor)
  std::unique_ptr<WsManager::Binding<T>> _wsBinding;

  bool _mounted{false};
};

#endif
