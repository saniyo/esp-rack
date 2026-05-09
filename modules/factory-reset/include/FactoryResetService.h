#ifndef FactoryResetService_h
#define FactoryResetService_h

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SecurityManager.h>
#include <RestartService.h>
#include <FS.h>

#define FS_CONFIG_DIRECTORY "/config"
#define FACTORY_RESET_SERVICE_PATH "/rest/factoryReset"

class WebManager;

class FactoryResetService {
  FS* fs;

 public:
  FactoryResetService(AsyncWebServer* server, FS* fs, SecurityManager* securityManager);

  // Publish the factory-reset endpoint via WebManager as action
  // `system.factoryReset`. Legacy URL is preserved.
  void registerManifest(WebManager* web);

  void factoryReset();

 private:
  void handleRequest(AsyncWebServerRequest* request);
};

#endif  // end FactoryResetService_h
