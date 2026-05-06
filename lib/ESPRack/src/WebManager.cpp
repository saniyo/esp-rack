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

  // Manifest is public — the frontend fetches it before sign-in to build the route
  // set. Per-entry handlers still enforce their own auth predicate.
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
void WebManager::serveManifest(AsyncWebServerRequest* request) {
  AsyncResponseStream* response = request->beginResponseStream("application/json");

  response->print(F("{\"schemaVersion\":2"));

  // device — small, fits trivially in 256 bytes.
  {
    StaticJsonDocument<256> dev;
    JsonObject d = dev.to<JsonObject>();
    d["name"]    = _deviceName;
    d["version"] = _deviceVersion;
    response->print(F(",\"device\":"));
    serializeJson(dev, *response);
  }

  // buildFeatures — bounded by the count of FT_* registrations done in
  // App ctor (currently ~10 keys at ~25 bytes each).
  {
    DynamicJsonDocument bf(1024);
    JsonObject o = bf.to<JsonObject>();
    for (const auto& b : _buildFeatures) {
      if (b.key) o[b.key] = b.enabled;
    }
    response->print(F(",\"buildFeatures\":"));
    serializeJson(bf, *response);
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
