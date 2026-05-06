#include <FactoryResetService.h>

#include <WebManager.h>

using namespace std::placeholders;

// Ctor kept signature-compatible with the pre-migration layout. The HTTP
// mount now happens through WebManager::registerAction (see registerManifest)
// so the action also shows up in the manifest.
FactoryResetService::FactoryResetService(AsyncWebServer* /*server*/, FS* fs,
                                         SecurityManager* /*securityManager*/)
    : fs(fs) {}

void FactoryResetService::registerManifest(WebManager* web) {
  if (!web) return;
  WebActionSpec spec;
  spec.id = "system.factoryReset";
  spec.title = "Factory Reset";
  spec.icon = "SettingsBackupRestore";
  spec.color = "error";
  spec.restPath = FACTORY_RESET_SERVICE_PATH;
  spec.method = "POST";
  spec.auth = WebAuthLevel::Admin;
  spec.confirm = "Are you sure you want to reset the device to its factory defaults?";
  spec.successMessage = "Device has been factory reset and will now restart";
  spec.handler = [this](AsyncWebServerRequest* request) { this->handleRequest(request); };
  web->registerAction(spec);
}

void FactoryResetService::handleRequest(AsyncWebServerRequest* request) {
  request->onDisconnect(std::bind(&FactoryResetService::factoryReset, this));
  request->send(200);
}

/**
 * Delete function assumes that all files are stored flat, within the config directory.
 */
void FactoryResetService::factoryReset() {
  File root = fs->open(FS_CONFIG_DIRECTORY);
  File file;
  while (file = root.openNextFile()) {
    String path = file.path();
    file.close();
    fs->remove(path);
  }
  RestartService::restartNow();
}
