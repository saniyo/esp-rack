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
  SystemStatus(AsyncWebServer* server, SecurityManager* securityManager);

  // Contribute the 'status' tab to the compound 'system' feature. ESPReact
  // registers the empty shell; each cooperating service adds its own tab
  // via WebManager::addTabToFeature() in its own registerManifest().
  void registerManifest(WebManager* web);

  // Builds the 'status' sub-form into `root`. Invoked by the status-tab
  // endpoint handler below.
  static void buildForm(JsonObject& root);

 private:
  void systemStatus(AsyncWebServerRequest* request);
  void systemStatusForm(AsyncWebServerRequest* request);
};

#endif  // end SystemStatus_h
