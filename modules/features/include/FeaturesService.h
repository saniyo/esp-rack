#ifndef FeaturesService_h
#define FeaturesService_h

#include <Features.h>

#ifdef ESP32
#include <WiFi.h>
#include <AsyncTCP.h>
#endif

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>

class WebManager;

#define MAX_FEATURES_SIZE 256
#define FEATURES_SERVICE_PATH "/rest/features"

class FeaturesService {
 public:
  // `web` is optional — when present the service ALSO registers the
  // build-flag JSON as a proxy endpoint on WebManager so the
  // mothership mship-ui reverse proxy (Phase 7c) can reach it via
  // rest.proxy. Same JSON shape either way.
  FeaturesService(AsyncWebServer* server, WebManager* web = nullptr);

  // Shared JSON builder — used by both the AsyncWebServer handler
  // and the WebManager proxy-endpoint handler. Writes the FT_*
  // build-flag map into root.
  static void buildFeaturesObject(JsonObject root);

 private:
  void features(AsyncWebServerRequest* request);
};

#endif
