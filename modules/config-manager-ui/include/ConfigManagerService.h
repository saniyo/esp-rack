#ifndef ConfigManagerService_h
#define ConfigManagerService_h

#include <ESPAsyncWebServer.h>
#include <SecurityManager.h>
#include <ConfigManager.h>

#define CONFIG_MGR_FORM_PATH    "/rest/system/configmgr"
#define CONFIG_MGR_BUFFER_SIZE  4096

class WebManager;

// Surface for the System → Config Manager tab.
//
// Mounts one read-only REST handler that returns a form describing every
// registered ConfigManager entry (id, primary file, on-disk size, save
// counter, snapshot ✓/✗, snapshot age) plus two action references:
//
//   * config.snapshot   — force-copy every primary into <primary>.snapshot
//                         (operator-explicit "freeze current configs as
//                         backup"). Same file the auto-rotate writes to;
//                         operator just gets to choose when "now" is.
//   * config.restore    — copy every <primary>.snapshot back over its
//                         primary, then reboot so each service's begin()
//                         picks the restored state up cleanly.
//
// Auto-recovery on boot from `.snapshot` (when primary is unreadable) is
// handled by ConfigManager::loadOrRecover and needs no operator gesture.
class ConfigManagerService {
 public:
  ConfigManagerService(AsyncWebServer* server,
                       SecurityManager* securityManager,
                       ConfigManager* configManager);

  void registerManifest(WebManager* web);

 private:
  AsyncWebServer*  _server{nullptr};
  SecurityManager* _sm{nullptr};
  ConfigManager*   _cfg{nullptr};

  void serveForm(AsyncWebServerRequest* request);
  void buildForm(JsonObject& root);
};

#endif
