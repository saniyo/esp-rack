#ifndef SystemStatus_h
#define SystemStatus_h

#include <WiFi.h>
#include <AsyncTCP.h>

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <SecurityManager.h>
#include <ESPFS.h>

// C1 (api-v1) bespoke page paths. The flat machine reader lives at
// /api/v1/system/health — NOT /api/v1/system/status — because
// AsyncWebServer prefix-matches a registered URI against "<uri>/..."
// subpaths: an endpoint at .../status would swallow .../status/form and
// the Resources tab would render the flat payload (observed live).
// Rule of thumb: a mounted endpoint path must never be a path-prefix of
// another endpoint; /form faces hang off non-endpoint parents only.
#define SYSTEM_STATUS_SERVICE_PATH   "/api/v1/system/health"
#define SYSTEM_STATUS_FORM_PATH      "/api/v1/system/status/form"
#define SYSTEM_IDENTITY_FORM_PATH    "/api/v1/system/identity/form"

class WebManager;

class SystemStatus {
 public:
  // `web` is optional — when supplied, the status form gains a Modules
  // table populated from WebManager's module-version registry plus a
  // framework-version row. Passing nullptr keeps the legacy hardware-
  // only status payload (chip / heap / flash / fs).
  SystemStatus(AsyncWebServer* server, SecurityManager* securityManager,
               WebManager* web = nullptr);

  // Contribute the 'status' tab to the compound 'system' feature. App
  // registers the empty shell; each cooperating service adds its own
  // tab via WebManager::addTabToFeature() in its own registerManifest.
  void registerManifest(WebManager* web);

  // Builds the 'status' sub-form into `root`. Non-static now because it
  // reads the WebManager* captured at construction to surface module
  // versions; passing _web through every call site would be noise.
  //
  // free_snap / max_snap — optional pre-captured heap values taken
  // at the request-handler entry point, BEFORE sendAdaptiveJson
  // allocates the response doc. Used to display the "Heap (live,
  // at request entry)" row. Pass 0 / 0 and the form falls back to
  // a live read inside buildForm (legacy path for callers that
  // haven't been updated).
  void buildForm(JsonObject& root,
                  uint32_t free_snap = 0,
                  uint32_t max_snap  = 0);

  // Builds the 'identity' sub-form into `root`. Pure DeviceIdentity
  // readout (project, MAC, eFuse UID, flash UID, canonical device
  // ID). Lives in its own tab so the Status tab stays focused on
  // runtime metrics (heap / fs / modules) and the immutable identity
  // doesn't get lost in the noise.
  void buildIdentityForm(JsonObject& root);

 private:
  AsyncWebServer*  _server{nullptr};
  SecurityManager* _sec{nullptr};
  WebManager*      _web{nullptr};

  void systemStatus(AsyncWebServerRequest* request);
  void systemStatusForm(AsyncWebServerRequest* request);
  void systemIdentityForm(AsyncWebServerRequest* request);
};

#endif  // end SystemStatus_h
