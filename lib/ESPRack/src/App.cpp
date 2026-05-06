#include "App.h"
#include "NullSecurityManager.h"
#include "ESPFS.h"
#include "Features.h"
#include "WebFeatureSpec.h"

#include <ESPAsyncWebServer.h>
#include <WiFi.h>

#ifdef PROGMEM_WWW
#include <WWWData.h>
#endif

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
    security_     {&nullSecurity()} {
  // Framework-owned UI shell — registered BEFORE Builder runs module
  // onInstall, so that modules calling addTabToFeature("system", ...)
  // find a parent. buildFeatures map exposed via /rest/uiManifest is
  // populated here so the manifest payload reflects build-time gates
  // even if no module touches them.
  webManager_.registerBuildFeature("project",         FT_ENABLED(FT_PROJECT));
  webManager_.registerBuildFeature("security",        FT_ENABLED(FT_SECURITY));
  webManager_.registerBuildFeature("websocket",       FT_ENABLED(FT_WEBSOCKET));
  webManager_.registerBuildFeature("mqtt",            FT_ENABLED(FT_MQTT));
  webManager_.registerBuildFeature("ntp",             FT_ENABLED(FT_NTP));
  webManager_.registerBuildFeature("ota",             FT_ENABLED(FT_OTA));
  webManager_.registerBuildFeature("upload_firmware", FT_ENABLED(FT_UPLOAD_FIRMWARE));
  webManager_.registerBuildFeature("telegram",        FT_ENABLED(FT_TELEGRAM));
  webManager_.registerBuildFeature("auto_update",     FT_ENABLED(FT_AUTO_UPDATE));
  webManager_.registerBuildFeature("system_info",     FT_ENABLED(FT_SYSTEM_INFO));

  WebFeatureSpec sys;
  sys.id            = "system";
  sys.title         = "System";
  sys.component     = "DynamicSettings";
  sys.menu.label    = "System";
  sys.menu.icon     = "Settings";
  sys.menu.order    = 900;
  sys.menu.auth     = WebAuthLevel::Authenticated;
  sys.auth          = WebAuthLevel::Authenticated;
  sys.routeTemplate = "/system/*";
  webManager_.registerCompoundFeature(std::move(sys));
}

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

  // 7. Static-file serving for the React UI bundle. Two paths:
  //    PROGMEM_WWW  — bundle baked into firmware as gzip-compressed
  //                   const arrays via build_interface.py. Each entry
  //                   gets its own GET handler; index.html is the
  //                   onNotFound fallback so the React router handles
  //                   client-side routes.
  //    !PROGMEM_WWW — serve straight from /www/ on LittleFS. Requires
  //                   the consumer to flash a data partition image
  //                   beforehand. Useful for development iteration so
  //                   PROGMEM_WWW rebuilds aren't on every change.
  AsyncWebServer* srv = server_;
#ifdef PROGMEM_WWW
  WWWData::registerRoutes(
      [srv](const String& uri, const String& contentType, const uint8_t* content, size_t len) {
        ArRequestHandlerFunction handler = [contentType, content, len](AsyncWebServerRequest* req) {
          AsyncWebServerResponse* res = req->beginResponse(200, contentType, content, len);
          res->addHeader("Content-Encoding", "gzip");
          req->send(res);
        };
        srv->on(uri.c_str(), HTTP_GET, handler);
        if (uri.equals("/index.html")) {
          srv->onNotFound([handler](AsyncWebServerRequest* req) {
            if (req->method() == HTTP_GET) {
              handler(req);
            } else if (req->method() == HTTP_OPTIONS) {
              req->send(200);
            } else {
              req->send(404);
            }
          });
        }
      });
#else
  srv->serveStatic("/js/",          ESPFS, "/www/js/");
  srv->serveStatic("/css/",         ESPFS, "/www/css/");
  srv->serveStatic("/fonts/",       ESPFS, "/www/fonts/");
  srv->serveStatic("/app/",         ESPFS, "/www/app/");
  srv->serveStatic("/favicon.ico",  ESPFS, "/www/favicon.ico");
  srv->onNotFound([](AsyncWebServerRequest* req) {
    if (req->method() == HTTP_GET) {
      req->send(ESPFS, "/www/index.html");
    } else if (req->method() == HTTP_OPTIONS) {
      req->send(200);
    } else {
      req->send(404);
    }
  });
#endif

  // 8. Start AsyncWebServer listening on port 80. Done LAST so every
  // module's registerFeature/registerAction + the static-file fallback
  // are already mounted before the first incoming request arrives.
  // The legacy ESPReact pattern delegated this call to the consumer's
  // main.cpp; ESPRack absorbs it because the consumer would just call
  // it right after app->begin() anyway, and forgetting it surfaces as
  // "192.168.4.1 doesn't load anything" with no actionable error.
  server_->begin();
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
