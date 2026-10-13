#ifndef FactoryResetService_h
#define FactoryResetService_h

#ifdef ESP32
#include <WiFi.h>
#include <AsyncTCP.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#endif

#include <ESPAsyncWebServer.h>
#include <SecurityManager.h>
#include <App.h>  // ESPRack::restartDevice (core) — not the restart module
#include <FS.h>

#define FS_CONFIG_DIRECTORY "/config"

class WebManager;

class FactoryResetService {
  FS* fs;

 public:
  FactoryResetService(AsyncWebServer* server, FS* fs, SecurityManager* securityManager);

  // Publish the factory-reset endpoint via WebManager as action
  // `system.factoryReset`. Registry mounts it at
  // /api/v1/actions/system.factoryReset automatically.
  void registerManifest(WebManager* web);

  void factoryReset();

 private:
  void handleRequest(AsyncWebServerRequest* request);
};

#endif  // end FactoryResetService_h
