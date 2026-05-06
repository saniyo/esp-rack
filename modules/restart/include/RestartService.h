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

#define RESTART_SERVICE_PATH "/rest/restart"

class WebManager;

class RestartService {
 public:
  RestartService(AsyncWebServer* server, SecurityManager* securityManager);

  // Publish the restart endpoint via WebManager: registers an action
  // `system.restart` at RESTART_SERVICE_PATH and emits manifest metadata so
  // forms can embed it via actionRef("system.restart"). The legacy URL is
  // preserved — only the registration path changes.
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
