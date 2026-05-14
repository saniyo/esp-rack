#include <WebManager.h>

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

  AsyncResponseStream* response = request->beginResponseStream("application/json");

  response->print(F("{\"schemaVersion\":2"));

  // device — small, fits trivially in 256 bytes.
  // Three keys: name + version (consumer-app identity from Builder),
  // plus frameworkVersion (esp-rack library rev from Version.h, set
  // by App.begin() via setFrameworkVersion).
  // ALWAYS in the response — the SignIn page needs the device name
  // for branding before the user has a token.
  {
    StaticJsonDocument<256> dev;
    JsonObject d = dev.to<JsonObject>();
    d["name"]    = _deviceName;
    d["version"] = _deviceVersion;
    if (_frameworkVersion && *_frameworkVersion) {
      d["frameworkVersion"] = _frameworkVersion;
    }
    response->print(F(",\"device\":"));
    serializeJson(dev, *response);
  }

  // buildFeatures — exposed in BOTH tiers because the SignIn shell
  // needs to know whether `security` is enabled (decides whether to
  // render the login form at all). Other build flags are not
  // sensitive — they're compile-time on/off knobs that any reverse-
  // engineer of the firmware binary can also see.
  {
    DynamicJsonDocument bf(1024);
    JsonObject o = bf.to<JsonObject>();
    for (const auto& b : _buildFeatures) {
      if (b.key) o[b.key] = b.enabled;
    }
    response->print(F(",\"buildFeatures\":"));
    serializeJson(bf, *response);
  }

  // Anonymous tier ends here. Authenticated tier continues with
  // modules + features (the actual endpoint map). The closing brace
  // is shared so the response is valid JSON either way.
  if (!authed) {
    response->print(F(",\"authenticated\":false}"));
    request->send(response);
    return;
  }
  response->print(F(",\"authenticated\":true"));

  // modules — id+version per installed module. Populated by App.begin()
  // by walking the modules vector and calling describe() on each. The
  // System status tab renders a table of these so operators can see at
  // a glance which framework component versions the running firmware
  // ships with — independent of the consumer-app version (deviceVersion)
  // and of any per-service WebFeatureSpec.version.
  {
    DynamicJsonDocument mods(2048);
    JsonArray arr = mods.to<JsonArray>();
    for (const auto& m : _modules) {
      JsonObject o = arr.createNestedObject();
      o["id"]      = m.id;
      o["version"] = m.version;
    }
    response->print(F(",\"modules\":"));
    serializeJson(mods, *response);
  }

  // features — the part that used to overflow. The doc is allocated
  // ONCE outside the loop and cleared between iterations: an N-entry
  // manifest used to do N*4 KB of malloc/free per request, which after
  // ~hundreds of frontend polls fragmented the heap badly enough to
  // surface as multi_heap poison-pattern crashes 5+ min into uptime.
  // Single allocation + clear() drops the per-request heap traffic to
  // a flat 4 KB regardless of entry count.
  response->print(F(",\"features\":["));
  bool first = true;
  DynamicJsonDocument doc(4096);
  for (const auto& e : _entries) {
    if (!e) continue;
    if (!first) response->print(',');
    first = false;
    doc.clear();
    JsonObject obj = doc.to<JsonObject>();
    e->toJson(obj);
    serializeJson(doc, *response);
  }
  response->print(F("]}"));

  request->send(response);
}
