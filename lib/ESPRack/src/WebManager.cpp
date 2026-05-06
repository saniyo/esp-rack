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

void WebManager::serveManifest(AsyncWebServerRequest* request) {
  AsyncJsonResponse* response = new AsyncJsonResponse(false, UI_MANIFEST_BUFFER_SIZE);
  JsonObject root = response->getRoot();

  root["schemaVersion"] = 2;

  JsonObject device = root.createNestedObject("device");
  device["name"] = _deviceName;
  device["version"] = _deviceVersion;

  JsonObject bf = root.createNestedObject("buildFeatures");
  for (const auto& b : _buildFeatures) {
    if (b.key) bf[b.key] = b.enabled;
  }

  JsonArray features = root.createNestedArray("features");
  for (const auto& e : _entries) {
    if (!e) continue;
    JsonObject obj = features.createNestedObject();
    e->toJson(obj);
  }

  response->setLength();
  request->send(response);
}
