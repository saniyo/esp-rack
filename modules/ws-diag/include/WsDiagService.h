#pragma once
#ifndef WsDiagService_h
#define WsDiagService_h

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <SecurityManager.h>
#include <WsManager.h>

#define WS_DIAG_FORM_PATH   "/rest/system/sockets"
#define WS_DIAG_BUFFER_SIZE 8192

class WebManager;
class PresenceService;

// Admin-only diagnostic surface: lists every WS endpoint and each active
// client with IP, age, and seconds since last inbound/outbound frame.
// Contributes a "Sockets" tab into the compound 'system' feature; the tab
// uses the standard DynamicSettings renderer (read-only, so no Save button).
class WsDiagService {
 public:
  WsDiagService(AsyncWebServer* server, SecurityManager* securityManager,
                WsManager* wsMgr, PresenceService* presence);

  void registerManifest(WebManager* web);

 private:
  AsyncWebServer*   _server{nullptr};
  SecurityManager*  _sm{nullptr};
  WsManager*        _ws{nullptr};
  PresenceService*  _presence{nullptr};

  void socketsForm(AsyncWebServerRequest* request);
  void buildForm(JsonObject& root);
};

#endif  // WsDiagService_h
