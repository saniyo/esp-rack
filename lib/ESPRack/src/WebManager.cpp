#include <WebManager.h>

#include <memory>

// True chunked streaming builder for /rest/uiManifest. Lives for the
// lifetime of ONE manifest response (owned by the AwsResponseFiller
// std::function), produces JSON on the fly section-by-section. Avoids
// the StreamString trap of AsyncResponseStream (which silently buffers
// the entire payload in heap and truncates on realloc failure under
// fragmentation pressure) AND the "build whole cache" trap (a 15+ KB
// std::string permanent allocation that itself fragments).
//
// Memory profile (per active manifest response, held in heap by the
// response object until the last chunk lands on the wire):
//   * sizeof(ManifestBuilder) ~ 5 KB (StaticJsonDocument<4096> +
//     itemBuf + state).
//   * No further allocations during fill — per-entry JSON renders
//     into the pre-allocated StaticJsonDocument pool + itemBuf.
//
// State machine: phases advance monotonically. Each phase emits a
// contiguous byte range; once exhausted, cursor moves to next phase.
// Concurrent manifest requests each get their own builder; OK.
class ManifestBuilder {
 public:
  enum class Phase : uint8_t {
    HEADER,            // {"schemaVersion":2
    DEVICE_OPEN,       // ,"device":
    DEVICE,            // <serialized device object>
    BUILDFEATURES_OPEN,// ,"buildFeatures":
    BUILDFEATURES,     // <serialized object>
    AUTH_FALSE_END,    // ,"authenticated":false}
    AUTH_TRUE,         // ,"authenticated":true
    MODULES_OPEN,      // ,"modules":[
    MODULES_ITEM,      // <serialized module>
    MODULES_SEP,       // , (between items)
    MODULES_CLOSE,     // ]
    FEATURES_OPEN,     // ,"features":[
    FEATURES_ITEM,     // <serialized feature>
    FEATURES_SEP,      // ,
    FEATURES_CLOSE,    // ]
    FOOTER,            // }
    DONE
  };

  ManifestBuilder(const WebManager& mgr, bool authed)
      : _mgr(&mgr), _authed(authed) {
    advanceTo(Phase::HEADER);
  }

  // Fill up to maxLen bytes into buf. Returns bytes written.
  // Return 0 to signal end of response.
  size_t fillNext(uint8_t* buf, size_t maxLen) {
    size_t written = 0;
    while (written < maxLen && _phase != Phase::DONE) {
      if (_cursor >= _itemLen) {
        // Current phase exhausted — advance.
        advancePhase();
        if (_phase == Phase::DONE) break;
        continue;
      }
      size_t remaining = _itemLen - _cursor;
      size_t take = (remaining < (maxLen - written)) ? remaining : (maxLen - written);
      std::memcpy(buf + written,
                  reinterpret_cast<const uint8_t*>(_itemBuf) + _cursor,
                  take);
      _cursor += take;
      written += take;
    }
    return written;
  }

 private:
  void setLiteral(const char* s) {
    size_t n = std::strlen(s);
    if (n >= sizeof(_itemBuf)) n = sizeof(_itemBuf) - 1;
    std::memcpy(_itemBuf, s, n);
    _itemBuf[n] = '\0';
    _itemLen = n;
    _cursor = 0;
  }

  void renderDocIntoItem() {
    _itemLen = serializeJson(_doc, _itemBuf, sizeof(_itemBuf));
    _cursor = 0;
    _doc.clear();
  }

  void advanceTo(Phase p) {
    _phase = p;
    _cursor = 0;
    _itemLen = 0;
    fillPhaseBuffer();
  }

  void advancePhase() {
    // Transitions are linear except for the auth fork after BUILDFEATURES,
    // and the array iteration phases that loop back to ITEM until the
    // array is exhausted.
    switch (_phase) {
      case Phase::HEADER:             advanceTo(Phase::DEVICE_OPEN); return;
      case Phase::DEVICE_OPEN:        advanceTo(Phase::DEVICE); return;
      case Phase::DEVICE:             advanceTo(Phase::BUILDFEATURES_OPEN); return;
      case Phase::BUILDFEATURES_OPEN: advanceTo(Phase::BUILDFEATURES); return;
      case Phase::BUILDFEATURES:
        advanceTo(_authed ? Phase::AUTH_TRUE : Phase::AUTH_FALSE_END);
        return;
      case Phase::AUTH_FALSE_END:     advanceTo(Phase::DONE); return;
      case Phase::AUTH_TRUE:          advanceTo(Phase::MODULES_OPEN); return;
      case Phase::MODULES_OPEN:
        _modIdx = 0;
        if (_mgr->_modules.empty()) { advanceTo(Phase::MODULES_CLOSE); return; }
        advanceTo(Phase::MODULES_ITEM);
        return;
      case Phase::MODULES_ITEM:
        _modIdx++;
        if (_modIdx < _mgr->_modules.size()) {
          advanceTo(Phase::MODULES_SEP);
        } else {
          advanceTo(Phase::MODULES_CLOSE);
        }
        return;
      case Phase::MODULES_SEP:        advanceTo(Phase::MODULES_ITEM); return;
      case Phase::MODULES_CLOSE:      advanceTo(Phase::FEATURES_OPEN); return;
      case Phase::FEATURES_OPEN:
        _featIdx = 0;
        // Skip null entries at the head of _entries.
        while (_featIdx < _mgr->_entries.size() && !_mgr->_entries[_featIdx]) {
          _featIdx++;
        }
        if (_featIdx >= _mgr->_entries.size()) {
          advanceTo(Phase::FEATURES_CLOSE);
        } else {
          advanceTo(Phase::FEATURES_ITEM);
        }
        return;
      case Phase::FEATURES_ITEM: {
        size_t next = _featIdx + 1;
        while (next < _mgr->_entries.size() && !_mgr->_entries[next]) next++;
        if (next < _mgr->_entries.size()) {
          _featIdx = next;
          advanceTo(Phase::FEATURES_SEP);
        } else {
          advanceTo(Phase::FEATURES_CLOSE);
        }
        return;
      }
      case Phase::FEATURES_SEP:       advanceTo(Phase::FEATURES_ITEM); return;
      case Phase::FEATURES_CLOSE:     advanceTo(Phase::FOOTER); return;
      case Phase::FOOTER:             advanceTo(Phase::DONE); return;
      case Phase::DONE:               return;
    }
  }

  void fillPhaseBuffer() {
    switch (_phase) {
      case Phase::HEADER:              setLiteral("{\"schemaVersion\":2"); return;
      case Phase::DEVICE_OPEN:         setLiteral(",\"device\":"); return;
      case Phase::BUILDFEATURES_OPEN:  setLiteral(",\"buildFeatures\":"); return;
      case Phase::AUTH_FALSE_END:      setLiteral(",\"authenticated\":false}"); return;
      case Phase::AUTH_TRUE:           setLiteral(",\"authenticated\":true"); return;
      case Phase::MODULES_OPEN:        setLiteral(",\"modules\":["); return;
      case Phase::MODULES_SEP:         setLiteral(","); return;
      case Phase::MODULES_CLOSE:       setLiteral("]"); return;
      case Phase::FEATURES_OPEN:       setLiteral(",\"features\":["); return;
      case Phase::FEATURES_SEP:        setLiteral(","); return;
      case Phase::FEATURES_CLOSE:      setLiteral("]"); return;
      case Phase::FOOTER:              setLiteral("}"); return;
      case Phase::DONE:                _itemLen = 0; _cursor = 0; return;

      case Phase::DEVICE: {
        JsonObject d = _doc.to<JsonObject>();
        d["name"]    = _mgr->_deviceName;
        d["version"] = _mgr->_deviceVersion;
        if (_mgr->_frameworkVersion && *_mgr->_frameworkVersion) {
          d["frameworkVersion"] = _mgr->_frameworkVersion;
        }
        renderDocIntoItem();
        return;
      }
      case Phase::BUILDFEATURES: {
        JsonObject o = _doc.to<JsonObject>();
        for (const auto& b : _mgr->_buildFeatures) {
          if (b.key) o[b.key] = b.enabled;
        }
        renderDocIntoItem();
        return;
      }
      case Phase::MODULES_ITEM: {
        const auto& m = _mgr->_modules[_modIdx];
        JsonObject o = _doc.to<JsonObject>();
        o["id"]      = m.id;
        o["version"] = m.version;
        renderDocIntoItem();
        return;
      }
      case Phase::FEATURES_ITEM: {
        JsonObject o = _doc.to<JsonObject>();
        _mgr->_entries[_featIdx]->toJson(o);
        renderDocIntoItem();
        return;
      }
    }
  }

  const WebManager* _mgr;
  bool   _authed;
  Phase  _phase{Phase::HEADER};
  size_t _modIdx{0};
  size_t _featIdx{0};
  size_t _cursor{0};
  size_t _itemLen{0};

  // Per-entry rendering buffer. 2 KB is enough for typical
  // WebFeatureSpec serializations (tabs + actions). If we ever hit a
  // feature whose JSON exceeds this, serializeJson returns the value
  // it WOULD have written without overflowing — we'd notice as a
  // truncated entry. Increase here if that becomes real.
  char _itemBuf[2048];

  // ArduinoJson pool reused across items. StaticJsonDocument lives
  // inline (no separate heap alloc); 4 KB pool is comfortable for
  // typical WebFeatureSpec contents — keys + tabs + actions arrays.
  StaticJsonDocument<4096> _doc;
};

WebManager::WebManager(AsyncWebServer* server,
                       SecurityManager* sm,
                       WsManager* wsManager,
                       const char* deviceName,
                       const char* deviceVersion)
    : _server(server),
      _sm(sm),
      _wsManager(wsManager),
      _deviceName(deviceName ? deviceName : ""),
      _deviceVersion(deviceVersion ? deviceVersion : "") {}

void WebManager::begin() {
  if (_begun || !_server) return;

  for (auto& e : _entries) {
    if (e) e->registerEndpoints(_server, _sm);
  }

  // Manifest endpoint is two-tier (auth-aware) — it ALWAYS responds, but
  // the payload depends on whether the request carries a valid JWT:
  //   * Anonymous → minimal stub: schemaVersion + device.{name,version,
  //     frameworkVersion} + buildFeatures.security. Enough for the
  //     SignIn page to render the right brand and decide whether to show
  //     a login form, no leak of the endpoint inventory.
  //   * Authenticated → full manifest: features[], modules[], all
  //     buildFeatures, action endpoints, etc. The whole map.
  // Per-entry endpoints continue to enforce their own predicates; the
  // manifest just controls how much *map* is exposed to anonymous
  // observers. ManifestLoader on the frontend re-fetches after sign-in
  // to swap stub for full content (see ManifestContext.reload()).
  ArRequestHandlerFunction handler =
      [this](AsyncWebServerRequest* request) { this->serveManifest(request); };
  _server->on(UI_MANIFEST_PATH, HTTP_GET, handler);

  _begun = true;
}

// Streamed manifest assembly. Earlier revisions buffered the whole
// payload into a single AsyncJsonResponse — fine at 2-3 modules,
// catastrophic past ~12 features because the fixed buffer silently
// truncated mid-stream and dropped trailing entries (which manifested
// as "menu items mysteriously missing"). Now we beginResponseStream()
// and serialize each entry into its own small per-entry document, so
// total manifest size is bounded only by the network pipe — adding
// modules costs zero buffer pressure.
//
// Per-entry doc is sized for the largest single feature spec we expect
// (full WebFeatureSpec with tabs + actions). 4 KB leaves comfortable
// headroom; if a single entry ever exceeded this we'd see truncation
// of THAT entry only, never of unrelated entries downstream.
// Single-shot manifest builder for the rest.proxy path. Browser-facing
// /rest/uiManifest stays on AsyncResponseStream — this is the in-process
// alternative that fills a pre-allocated JsonObject so Mothership can
// snapshot the device's UI schema without an HTTPS round trip on each
// poll.
void WebManager::buildManifestObject(JsonObject root) {
  root["schemaVersion"] = 2;

  JsonObject device = root.createNestedObject("device");
  device["name"]    = _deviceName;
  device["version"] = _deviceVersion;
  if (_frameworkVersion && *_frameworkVersion) {
    device["frameworkVersion"] = _frameworkVersion;
  }

  JsonObject bf = root.createNestedObject("buildFeatures");
  for (const auto& b : _buildFeatures) {
    if (b.key) bf[b.key] = b.enabled;
  }

  // Trust boundary is the caller's mTLS handshake — always emit full
  // authenticated tier.
  root["authenticated"] = true;

  JsonArray modules = root.createNestedArray("modules");
  for (const auto& m : _modules) {
    JsonObject mo = modules.createNestedObject();
    mo["id"]      = m.id;
    mo["version"] = m.version;
  }

  JsonArray features = root.createNestedArray("features");
  for (const auto& e : _entries) {
    if (!e) continue;
    JsonObject fo = features.createNestedObject();
    e->toJson(fo);
  }
}

void WebManager::serveManifest(AsyncWebServerRequest* request) {
  // Auth probe — ALWAYS replies, never 401s here. The result decides
  // payload depth: anonymous = stub; authenticated = full. Real
  // endpoints behind the manifest still 401 on their own.
  bool authed = true;  // defaults to authed when SecurityManager isn't wired
                       // (NullSecurityManager or null pointer — both treat
                       // every request as admin in the legacy semantic).
  if (_sm) {
    Authentication auth = _sm->authenticateRequest(request);
    authed = auth.authenticated;
  }

  // True chunked streaming. The ManifestBuilder generates JSON bytes
  // section-by-section on demand; AsyncWebServer's TCP write loop
  // invokes the filler repeatedly until it returns 0. No StreamString,
  // no cache, no Content-Length — payload size can be arbitrary
  // without pre-allocating a buffer large enough to hold it.
  //
  // Builder is owned by a shared_ptr that the lambda captures by
  // value. shared_ptr is copyable (unlike unique_ptr), which matters
  // because AwsResponseFiller = std::function and std::function
  // requires its target to be copyable in some libstdc++ versions.
  // Lifetime ends when the response object releases the lambda
  // (after the last chunk lands on the wire), which destroys the
  // shared_ptr and frees the builder.
  auto builder = std::make_shared<ManifestBuilder>(*this, authed);

  AsyncWebServerResponse* response = request->beginChunkedResponse(
      "application/json",
      [builder](uint8_t* buf, size_t maxLen, size_t /*index*/) -> size_t {
        return builder->fillNext(buf, maxLen);
      });
  request->send(response);
}
