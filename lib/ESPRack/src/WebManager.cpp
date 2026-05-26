#include <WebManager.h>

// ── ManifestBuilder method definitions ─────────────────────────────
// The class itself is declared in WebManager.h so the singleton
// instance can live as a WebManager member (BSS, not per-request
// heap). Method bodies stay here to keep the header lean and
// recompiles cheap when state-machine logic changes.

void ManifestBuilder::reset(const WebManager& mgr, bool authed) {
  _mgr     = &mgr;
  _authed  = authed;
  _modIdx  = 0;
  _featIdx = 0;
  _cursor  = 0;
  _itemLen = 0;
  _doc.clear();
  advanceTo(Phase::HEADER);
}

size_t ManifestBuilder::fillNext(uint8_t* buf, size_t maxLen) {
  size_t written = 0;
  while (written < maxLen && _phase != Phase::DONE) {
    if (_cursor >= _itemLen) {
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

void ManifestBuilder::setLiteral(const char* s) {
  size_t n = std::strlen(s);
  if (n >= sizeof(_itemBuf)) n = sizeof(_itemBuf) - 1;
  std::memcpy(_itemBuf, s, n);
  _itemBuf[n] = '\0';
  _itemLen = n;
  _cursor = 0;
}

void ManifestBuilder::renderDocIntoItem() {
  _itemLen = serializeJson(_doc, _itemBuf, sizeof(_itemBuf));
  _cursor = 0;
  _doc.clear();
}

void ManifestBuilder::advanceTo(Phase p) {
  _phase = p;
  _cursor = 0;
  _itemLen = 0;
  fillPhaseBuffer();
}

void ManifestBuilder::advancePhase() {
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

void ManifestBuilder::fillPhaseBuffer() {
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

  // Create the manifest-builder serialisation mutex on first begin().
  // FreeRTOS mutex is ~80 bytes one-time heap; created at boot when
  // heap is uncontested. Lives for the lifetime of the program.
  if (!_manifestMutex) {
    _manifestMutex = xSemaphoreCreateMutex();
  }

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
  // OOM guard. ManifestBuilder is ~3 KB (StaticJsonDocument<2048> +
  // 1 KB itemBuf + state). std::make_shared adds ~32 B control block.
  // Under -fno-exceptions a std::bad_alloc would call std::terminate
  // and reboot the device. Pre-check max_alloc so we 503 instead.
  if (ESP.getMaxAllocHeap() < 4096) {
    log_w("[web.manifest] heap pressure (max_alloc=%u) — 503 retry",
          (unsigned)ESP.getMaxAllocHeap());
    request->send(503, "application/json",
                  "{\"error\":\"heap_pressure\",\"retryAfter\":2}");
    return;
  }

  // Auth probe — ALWAYS replies, never 401s here. The result decides
  // payload depth: anonymous = stub; authenticated = full. Real
  // endpoints behind the manifest still 401 on their own.
  bool authed = true;
  if (_sm) {
    Authentication auth = _sm->authenticateRequest(request);
    authed = auth.authenticated;
  }

  // Per-request builder so concurrent fetches (e.g. browser refetches
  // after login while the anonymous fetch is still streaming) don't
  // share state. Singleton + mutex was attempted but AsyncWebServer's
  // chunked-response lifecycle does not enforce serial fillNext calls
  // strictly enough — both lambdas can be in flight on the SAME
  // builder at once, corrupting the state machine. With per-request
  // allocation each fetch gets its own ~3 KB builder, isolated.
  //
  // Captured by shared_ptr (copyable; std::function requires that)
  // and released on the lambda's destruction when the response object
  // is torn down by AsyncWebServer.
  auto builder = std::make_shared<ManifestBuilder>();
  builder->reset(*this, authed);

  AsyncWebServerResponse* response = request->beginChunkedResponse(
      "application/json",
      [builder](uint8_t* buf, size_t maxLen, size_t /*index*/) -> size_t {
        return builder->fillNext(buf, maxLen);
      });
  request->send(response);
}
