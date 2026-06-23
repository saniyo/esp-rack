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

#define FEATURES_SERVICE_PATH "/api/v1/features"

class FeaturesService {
 public:
  // `web` is optional — when present the service ALSO registers the
  // build-flag JSON as a proxy endpoint on WebManager so the
  // mothership mship-ui reverse proxy (Phase 7c) can reach it via
  // rest.proxy. Same JSON shape either way.
  FeaturesService(AsyncWebServer* server, WebManager* web = nullptr);

  // Shared JSON builder — used by both the AsyncWebServer handler and the
  // WebManager proxy-endpoint handler. Module-installed features are derived
  // from WebManager's install map (the same source as manifest.buildFeatures,
  // populated in App::begin from the .install<>() chain), so feature on/off
  // tracks what's installed rather than per-module FT_* macros. FT_SECURITY
  // (a core compile-out flag, not a standalone module) still gates `security`.
  static void buildFeaturesObject(JsonObject root, WebManager* web);

 private:
  void features(AsyncWebServerRequest* request);
  WebManager* _web{nullptr};
};

#endif
