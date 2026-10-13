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
#include <App.h>   // ESPRack::restartDevice

class WebManager;

class RestartService {
 public:
  RestartService(AsyncWebServer* server, SecurityManager* securityManager);

  // Publish the restart endpoint via WebManager: registers action
  // `system.restart`. The registry mounts it at
  // /api/v1/actions/system.restart automatically; spec.restPath is
  // ignored and no path #define is needed.
  void registerManifest(WebManager* web);

  // Graceful restart through the framework (LIB-FIX-006): every module's
  // onShutdown() runs (reverse order, on the loop task) before the reset.
  static void restartNow(const char* reason = ESPRack::kRestartUser) {
    ESPRack::restartDevice(reason);
  }

 private:
  void restart(AsyncWebServerRequest* request);
};

#endif  // end RestartService_h
