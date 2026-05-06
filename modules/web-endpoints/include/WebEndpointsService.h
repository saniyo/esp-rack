#ifndef WebEndpointsService_h
#define WebEndpointsService_h

#include <ESPAsyncWebServer.h>
#include <SecurityManager.h>

#define WEB_ENDPOINTS_FORM_PATH    "/rest/system/endpoints"
#define WEB_ENDPOINTS_BUFFER_SIZE  8192

class WebManager;
class WsManager;

// Surface for the System → Endpoints tab.
//
// Mirrors the Config Manager tab pattern but reads from WebManager's
// registry instead of ConfigManager's. Returns a read-only form with:
//   * a summary line — total entries + breakdown by Kind
//   * a per-entry table — id, kind, REST read path, REST update path,
//     WS path, HTTP method, auth level
//
// No actions; this tab is purely informational. The data source is
// IWebFeatureEntry::toJson() (the same method that backs /rest/uiManifest)
// so what operators see here matches what the SPA's manifest sees, just
// flattened into a table.
class WebEndpointsService {
 public:
  WebEndpointsService(AsyncWebServer* server,
                      SecurityManager* securityManager,
                      WebManager* webManager,
                      WsManager* wsManager);

  void registerManifest(WebManager* web);

 private:
  AsyncWebServer*  _server{nullptr};
  SecurityManager* _sm{nullptr};
  WebManager*      _wm{nullptr};
  WsManager*       _ws{nullptr};

  void serveForm(AsyncWebServerRequest* request);
  void buildForm(JsonObject& root);
};

#endif
