#include "App.h"
#include "NullSecurityManager.h"
#include "ESPFS.h"

#include <ESPAsyncWebServer.h>
#include <WiFi.h>

namespace ESPRack {

namespace {
// One per process — leaked on purpose. NullSecurityManager has no
// destructor side effects and lives as long as the App that points to
// it. Cleaner than threading ownership through every code path.
NullSecurityManager& nullSecurity() {
  static NullSecurityManager s;
  return s;
}
}  // namespace

App::App(AsyncWebServer* server, const char* deviceName, const char* deviceVersion) :
    server_       {server},
    deviceName_   {deviceName ? deviceName : ""},
    deviceVersion_{deviceVersion ? deviceVersion : ""},
    configManager_{&ESPFS},
    wsManager_    {server, 10},
    webManager_   {server,
                   &nullSecurity(),
                   &wsManager_,
                   deviceName,
                   deviceVersion},
    security_     {&nullSecurity()} {}

App::~App() {
  if (begun_) shutdown();
}

void App::adoptModules(std::vector<std::unique_ptr<Module>>&& modules) {
  modules_ = std::move(modules);
}

void App::begin() {
  if (begun_) return;
  begun_ = true;

  // 1. Filesystem first — ConfigManager and several modules need /config
  // available before they can ensureLoaded().
  ESPFS.begin(true);

  // 2. ConfigManager picks up its work directories under /config.
  configManager_.begin();

  // 3. Bring up lwIP early — modules that touch UDP/TCP in onBegin
  // (NTP, OTA, Telegram) crash on Arduino-3.x / IDF 5 if WiFi.mode
  // hasn't run yet. AP_STA performs the heavy "Wi-Fi driver +
  // network interfaces" init in one shot regardless of which
  // module(s) actually use STA vs AP.
  WiFi.mode(WIFI_AP_STA);

  // 4. Modules' onInstall has already happened in Builder.build();
  // here we run their onBegin in the same install order.
  for (auto& m : modules_) {
    if (m) m->onBegin();
  }

  // 5. WebManager.begin() iterates registered features and mounts
  // /rest/uiManifest. Must come AFTER all modules have registered.
  webManager_.begin();

  // 6. WS keepalive — phantom-client eviction, defaults match the
  // legacy ESPReact values.
  wsManager_.beginPingPong(20, 60000);
}

void App::loop() {
  if (!begun_) return;
  for (auto& m : modules_) {
    if (m) m->onLoop();
  }
  wsManager_.processAllQueues();
}

void App::shutdown() {
  if (!begun_) return;
  begun_ = false;
  // Reverse install order so dependents tear down first.
  for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
    if (*it) (*it)->onShutdown();
  }
}

}  // namespace ESPRack
