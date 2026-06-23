#include <FeaturesService.h>
#include <AdaptiveJson.h>
#include <WebManager.h>

FeaturesService::FeaturesService(AsyncWebServer* server, WebManager* web)
    : _web(web) {
  server->on(FEATURES_SERVICE_PATH, HTTP_GET, std::bind(&FeaturesService::features, this, std::placeholders::_1));

  // Phase 7c — mship-ui proxy can't reach paths registered directly
  // on AsyncWebServer (rest.proxy walks WebManager entries only).
  // Register an in-process handler that builds the same JSON so the
  // operator browser sees /api/v1/features through the reverse proxy.
  if (web) {
    web->registerProxyEndpoint(
        "GET", FEATURES_SERVICE_PATH,
        [web](JsonVariant /*body*/, JsonVariant out, int& out_status) {
          buildFeaturesObject(out.to<JsonObject>(), web);
          out_status = 200;
          return true;
        });
  }
}

// Module-installed features are derived from WebManager's install map (the
// same source App::begin uses for manifest.buildFeatures, keyed by each
// installed module's describe().id, plus the consumer "project" flag) — so
// enabling/disabling one is purely whether its module is .install<>()'d; the
// per-module FT_* compile flags are gone. FT_SECURITY stays a compile-out
// flag (it swaps the core SecurityManager for NullSecurityManager rather than
// gating a standalone module), so the `security` key still reflects it.
void FeaturesService::buildFeaturesObject(JsonObject root, WebManager* web) {
  auto installed = [web](const char* moduleId) -> bool {
    return web && web->hasBuildFeature(moduleId);
  };
  root["project"]         = installed("project");
  root["telegram"]        = installed("telegram");
  root["mqtt"]            = installed("mqtt");
  root["ntp"]             = installed("ntp");
  root["ota"]             = installed("ota");
  root["upload_firmware"] = installed("uploadFirmware");
  root["auto_update"]     = installed("autoUpdate");
  root["system_info"]     = installed("systemStatus");
#if FT_ENABLED(FT_SECURITY)
  root["security"] = true;
#else
  root["security"] = false;
#endif
}

void FeaturesService::features(AsyncWebServerRequest* request) {
  WebManager* web = _web;
  sendAdaptiveJson(request, [web](JsonObject& root) {
    buildFeaturesObject(root, web);
  }, 512);
}
