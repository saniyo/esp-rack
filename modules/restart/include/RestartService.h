#ifndef RestartService_h
#define RestartService_h

#ifdef ESP32
#include <WiFi.h>
#include <AsyncTCP.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#endif

#include <ESPAsyncWebServer.h>
#include <SecurityManager.h>

class WebManager;

class RestartService {
 public:
  RestartService(AsyncWebServer* server, SecurityManager* securityManager);

  // Publish the restart endpoint via WebManager: registers action
  // `system.restart`. The registry mounts it at
  // /api/v1/actions/system.restart automatically; spec.restPath is
  // ignored and no path #define is needed.
  void registerManifest(WebManager* web);

  static void restartNow() {
    WiFi.disconnect(true);
    // delay(500);
    vTaskDelay(500 / portTICK_PERIOD_MS);
    ESP.restart();
  }

 private:
  void restart(AsyncWebServerRequest* request);
};

#endif  // end RestartService_h
