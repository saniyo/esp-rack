#ifndef WebFeatureDelegate_h
#define WebFeatureDelegate_h

#include <cstring>
#include <memory>
#include <utility>

#include <HttpEndpoint.h>
#include <WsManager.h>

#include <IWebFeatureEntry.h>
#include <SecretFields.h>
#include <UpdateError.h>
#include <WriteAuth.h>
#include <WebFeatureSpec.h>

// Typed aggregator for a single StatefulService<T>: REST (GET/POST) + optional
// WS + manifest metadata. The service stores a raw pointer to the entry (via
// WebManager::registerFeature) and uses broadcastWs() to push state over WS.
// WebManager owns the lifetime through std::unique_ptr<IWebFeatureEntry>.

// SES-LIB-033 (WebManager.cpp, once for all features): a form without its
// values, and the GET <form>?schema=1 response built from it with ETag/304.
void stripFormValues(JsonObject root);
void sendFormSchema(AsyncWebServerRequest* r, const char* id, size_t bufSize,
                    const std::function<void(JsonObject&)>& fill);

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
  const char* menuHw() const override { return _spec.menu.hw; }

  void registerEndpoints(AsyncWebServer* server, SecurityManager* sm) override {
    if (_mounted || !server || !_service) return;

    // One boot-time probe of the FORM tree (heap fresh, never per request):
    //  * auto-sizes the REST buffer when the feature opted in
    //    (WEBFEATURE_BUFFER_AUTO); the state endpoint inherits it (C1);
    //  * collects the feature's secret keys (web::Secret fields) for the
    //    masking below (LIB-FIX-004);
    //  * maps every value key to its tab's write level (LIB-FIX-011).
    const size_t measured = probeForm();
    if (_restBufferSize == WEBFEATURE_BUFFER_AUTO) {
      _restBufferSize = measured;
    }
    if (!_writeAuthBuilt) {
      // Probe failed: no key map — WriteAuthMap fails closed (strictest tab).
      _writeAuth.build(JsonObjectConst(), _spec.tabs, _spec.auth, featureWriteAuth());
      _writeAuthBuilt = true;
    }

    // Secret-aware faces (LIB-FIX-004). Readers: secrets masked (always for
    // echoes/WS, for non-admins on GETs). Updaters: a value equal to the mask
    // is dropped so a round-tripped form can't overwrite the stored secret.
    _maskedEcho = [this](T& s, JsonObject& o) {
      stateReaderRaw()(s, o);
      SecretFields::maskAll(o, _secretKeys);
    };
    _postUpdater = [this](JsonObject& in, T& s) {
      SecretFields::stripMasked(in, _secretKeys);
      return _restUpdater(in, s);
    };
    _wsOut = [this](T& s, JsonObject& o) {
      _wsReader(s, o);
      SecretFields::maskAll(o, _secretKeys);
    };
    _wsIn = [this](JsonObject& in, T& s) {
      SecretFields::stripMasked(in, _secretKeys);
      return _wsUpdater(in, s);
    };

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
      // the form endpoint never needs an updater. Secrets masked for
      // non-admin identities.
      mountGet(server, sm, predicate, _formPath, /*formFace=*/true);
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
      auto writePredicate = [predicate](Authentication& a) {
        return predicate(a) && AuthenticationPredicates::IS_OPERATOR(a);
      };
      mountGet(server, sm, predicate, _statePath, /*formFace=*/false);
      mountStatePost(server, sm, writePredicate);
    }

    // Heap-allocate the Binding so its address never changes. Its ctor registers
    // an update handler that captures `this` — if we stored by value and moved,
    // that lambda would dangle.
    // Channel multiplex: every live feature shares ONE /ws socket; the channel
    // name is the feature id (NOT the legacy wsPath). _spec.wsPath now serves
    // only as the opt-in "this feature is live" flag.
    if (_spec.wsPath && _wsManager && !_wsBinding) {
      // The channel carries the feature's auth level: WsManager gates sub
      // by it and writes by it + the operator floor (LIB-FIX-003).
      // Pushes are secret-masked; writes drop round-tripped masks.
      _wsBinding.reset(new WsManager::Binding<T>(_wsManager, _spec.id, _wsBufferSize,
                                                 _service, _wsOut, _wsIn, true,
                                                 _spec.auth));
      // Per-key write levels (LIB-FIX-011) for WS "p" frames.
      if (_writeAuth.elevated()) {
        _wsManager->setChannelWriteLevel(_spec.id, [this](JsonObjectConst p) {
          return _writeAuth.required(p);
        });
      }
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
      if (_spec.menu.hw) menu["hw"] = _spec.menu.hw;
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
    // Live features advertise a CHANNEL id (multiplexed over the single /ws),
    // not a per-feature socket path. The frontend subscribes by `ch`.
    if (_spec.wsPath) obj["ch"] = _spec.id;
    if (_spec.staticSchema) obj["ss"] = 1;  // SES-LIB-033

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
      if (t.statePath) jt["statePath"] = t.statePath;  // SES-LIB-033
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
      ESPRack::clearUpdateError();
      StateUpdateResult outcome =
          _service->update(in_obj, _postUpdater ? _postUpdater : _restUpdater, "rest.proxy");
      if (outcome == StateUpdateResult::ERROR) {
        out_status = 400;
        ESPRack::writeUpdateRejected(out_body);
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
    return _wsManager->hasClients(_spec.id);   // channel == feature id
  }

  StatefulService<T>* service() const { return _service; }
  const WebFeatureSpec& spec() const { return _spec; }

 private:
  WebAuthLevel featureWriteAuth() const {
    return _spec.hasWriteAuth ? _spec.writeAuth : _spec.auth;
  }

  // POST /state: feature level + operator floor (predicate), then the
  // per-key write levels (LIB-FIX-011), then the update (masks stripped),
  // the masked echo, and the update handlers - the HttpPostEndpoint flow.
  void mountStatePost(AsyncWebServer* server, SecurityManager* sm,
                      AuthenticationPredicate writePredicate) {
    ArJsonRequestHandlerFunction fn = [this, sm](AsyncWebServerRequest* r, JsonVariant& json) {
      if (!json.is<JsonObject>()) {
        sendProblem(r, 400, "bad_request", "JSON object expected");
        return;
      }
      JsonObject in = json.as<JsonObject>();
      if (_writeAuth.elevated() && sm) {
        const WebAuthLevel need = _writeAuth.required(in);
        Authentication a = sm->authenticateRequest(r);
        const WebAuthLevel have = !a.authenticated ? WebAuthLevel::Public
                                : (a.user && a.user->admin) ? WebAuthLevel::Admin
                                : WebAuthLevel::Authenticated;
        if (!WriteAuthMap::atLeast(have, need)) {
          sendProblem(r, 403, "forbidden", "These settings need a higher access level to change");
          return;
        }
      }
      ESPRack::clearUpdateError();
      const StateUpdateResult outcome = _service->updateWithoutPropagation(in, _postUpdater);
      if (outcome == StateUpdateResult::ERROR) {
        ESPRack::sendUpdateRejected(r);
        return;
      }
      // Echo (masked). The change is already in RAM, so the update handlers
      // (save, apply, broadcast) run even when the echo had to be shed with a
      // 503: the client's retry then sees UNCHANGED and gets the echo. Skipping
      // them left the new value in RAM, unsaved and unapplied, until a reboot.
      sendAdaptiveJson(r, [this](JsonObject& o) { _service->read(o, _maskedEcho); },
                       _restBufferSize ? _restBufferSize : DEFAULT_BUFFER_SIZE);
      if (outcome == StateUpdateResult::CHANGED) {
        _service->callUpdateHandlers(HTTP_ENDPOINT_ORIGIN_ID);
      }
    };
    _postHandler.reset(new AsyncCallbackJsonWebHandler(
        _statePath, sm ? sm->wrapCallback(fn, writePredicate) : fn,
        _restBufferSize ? _restBufferSize : DEFAULT_BUFFER_SIZE));
    _postHandler->setMethod(HTTP_POST);
    server->addHandler(_postHandler.get());
  }

  // The reader behind the machine /state face: the distinct flat reader when
  // the feature registered one, else the form reader (interim).
  const JsonStateReader<T>& stateReaderRaw() const {
    return _hasDistinctStateReader ? _wsReader : _restReader;
  }

  // SES-LIB-033: the form without its values, static per firmware, so it
  // revalidates like the manifest (ETag = signature + id, 304).
  void sendSchema(AsyncWebServerRequest* r) {
    sendFormSchema(r, _spec.id, _restBufferSize ? _restBufferSize : DEFAULT_BUFFER_SIZE,
                   [this](JsonObject& root) { _service->read(root, _restReader); });
  }

  // GET for the form or state face. Secrets are masked unless the caller is
  // an admin; the identity check (one JWT verify) is skipped entirely for
  // features without secret fields.
  void mountGet(AsyncWebServer* server, SecurityManager* sm,
                AuthenticationPredicate predicate, const String& path, bool formFace) {
    ArRequestHandlerFunction handler = [this, sm, formFace](AsyncWebServerRequest* r) {
      if (formFace && _spec.staticSchema && r->hasParam("schema")) {
        sendSchema(r);
        return;
      }
      bool mask = false;
      if (!_secretKeys.empty() && sm) {
        Authentication a = sm->authenticateRequest(r);
        mask = !(a.authenticated && a.user && a.user->admin);
      }
      const JsonStateReader<T>& reader = formFace ? _restReader : stateReaderRaw();
      sendAdaptiveJson(
          r,
          [this, &reader, mask](JsonObject& root) {
            _service->read(root, reader);
            if (mask) SecretFields::maskAll(root, _secretKeys);
          },
          _restBufferSize ? _restBufferSize : DEFAULT_BUFFER_SIZE);
    };
    server->on(path.c_str(), HTTP_GET, sm ? sm->wrapRequest(handler, predicate) : handler);
  }

  // Build the REST tree once into a generous temp doc: size the runtime
  // buffer to the ACTUAL pool used (+512 B) — replaces hand-guessed
  // worst-case constants — and collect the feature's secret keys. Called
  // once at boot (heap fresh); the temp doc is freed on return. Safe
  // fallback if the probe can't allocate or the feature has no reader.
  size_t probeForm() {
    static constexpr size_t kProbe    = 24576;  // > any realistic schema
    static constexpr size_t kFallback = 8192;
    if (!_service || !_restReader) return kFallback;
    DynamicJsonDocument probe(kProbe);
    if (probe.capacity() == 0) return kFallback;            // boot heap too tight
    JsonObject root = probe.to<JsonObject>();
    if (root.isNull()) return kFallback;
    _service->read(root, _restReader);
    {
      JsonObjectConst rootC = root;
      SecretFields::collectFromForm(rootC, _secretKeys);
      // An overflowed probe has a truncated tree: leave the key map unbuilt
      // so registerEndpoints fails it closed.
      if (!probe.overflowed()) {
        _writeAuth.build(rootC, _spec.tabs, _spec.auth, featureWriteAuth());
        _writeAuthBuilt = true;
      }
    }
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

  // Secret handling (LIB-FIX-004): keys found by probeForm(), plus the
  // masking/stripping wrappers built from them in registerEndpoints.
  std::vector<String> _secretKeys;
  JsonStateReader<T>  _maskedEcho;   // POST /state echo
  JsonStateUpdater<T> _postUpdater;  // POST /state + rest.proxy writes
  JsonStateReader<T>  _wsOut;        // WS pushes / snapshots
  JsonStateUpdater<T> _wsIn;         // WS "p" writes

  // Server-side write levels per key (LIB-FIX-011), built by probeForm().
  WriteAuthMap _writeAuth;
  bool _writeAuthBuilt{false};
  std::unique_ptr<AsyncCallbackJsonWebHandler> _postHandler;  // state POST
  std::unique_ptr<WsManager::Binding<T>> _wsBinding;

  bool _mounted{false};
};

#endif
