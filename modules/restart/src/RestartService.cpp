#include <RestartService.h>

#include <WebManager.h>

// The ctor used to mount the REST handler directly. Now mounting happens
// via WebManager::registerAction (see registerManifest below) so the action
// appears in the manifest. Keep the ctor signature for compatibility.
RestartService::RestartService(AsyncWebServer* /*server*/, SecurityManager* /*securityManager*/) {}

void RestartService::registerManifest(WebManager* web) {
  if (!web) return;
  WebActionSpec spec;
  spec.id = "system.restart";
  spec.title = "Restart";
  spec.icon = "PowerSettingsNew";
  spec.color = "primary";
  // restPath not set — registry mounts at /api/v1/actions/system.restart.
  spec.method = "POST";
  spec.auth = WebAuthLevel::Admin;
  spec.confirm = "Are you sure you want to restart the device?";
  spec.successMessage = "Device is restarting";
  spec.handler = [this](AsyncWebServerRequest* request) { this->restart(request); };
  web->registerAction(spec);
}

void RestartService::restart(AsyncWebServerRequest* request) {
  request->onDisconnect([]() { RestartService::restartNow("system.restart"); });
  request->send(200);
}
