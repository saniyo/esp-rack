#include <RestartService.h>

#include <WebManager.h>

// The ctor used to mount the REST handler directly. Now mounting happens
// via WebManager::registerAction (see registerManifest below) so the action
// also appears in /rest/uiManifest. Keep the ctor signature so ESPReact's
// member init list stays untouched.
RestartService::RestartService(AsyncWebServer* /*server*/, SecurityManager* /*securityManager*/) {}

void RestartService::registerManifest(WebManager* web) {
  if (!web) return;
  WebActionSpec spec;
  spec.id = "system.restart";
  spec.title = "Restart";
  spec.icon = "PowerSettingsNew";
  spec.color = "primary";
  spec.restPath = RESTART_SERVICE_PATH;   // keep legacy URL for backward compat
  spec.method = "POST";
  spec.auth = WebAuthLevel::Admin;
  spec.confirm = "Are you sure you want to restart the device?";
  spec.successMessage = "Device is restarting";
  spec.handler = [this](AsyncWebServerRequest* request) { this->restart(request); };
  web->registerAction(spec);
}

void RestartService::restart(AsyncWebServerRequest* request) {
  request->onDisconnect(RestartService::restartNow);
  request->send(200);
}
