#include <FactoryResetService.h>

#include <WebManager.h>

using namespace std::placeholders;

// Ctor kept signature-compatible with the pre-migration layout. The HTTP
// mount now happens through WebManager::registerAction (see registerManifest)
// so the action appears in the manifest at /api/v1/actions/system.factoryReset.
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
  // restPath not set — registry mounts at /api/v1/actions/system.factoryReset.
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
 * Wipe /config and restart. The wipe runs AFTER every module's onShutdown()
 * (restartDevice's afterShutdown step, LIB-FIX-006), so a module that
 * persists state on shutdown can't resurrect config files on a reset device.
 * Delete function assumes that all files are stored flat, within the config
 * directory.
 */
void FactoryResetService::factoryReset() {
  FS* f = fs;
  ESPRack::restartDevice(ESPRack::kRestartFactoryReset, [f]() {
    File root = f->open(FS_CONFIG_DIRECTORY);
    File file;
    while (file = root.openNextFile()) {
      String path = file.path();
      file.close();
      f->remove(path);
    }
  });
}
