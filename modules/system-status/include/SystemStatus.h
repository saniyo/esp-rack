#ifndef SystemStatus_h
#define SystemStatus_h

#include <WiFi.h>
#include <AsyncTCP.h>

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <SecurityManager.h>
#include <ESPFS.h>

#define MAX_ESP_STATUS_SIZE 1024
#define SYSTEM_STATUS_SERVICE_PATH "/rest/systemStatus"
#define SYSTEM_STATUS_FORM_PATH    "/rest/system/status"

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
  void buildForm(JsonObject& root);

 private:
  WebManager* _web{nullptr};

  void systemStatus(AsyncWebServerRequest* request);
  void systemStatusForm(AsyncWebServerRequest* request);
};

#endif  // end SystemStatus_h
