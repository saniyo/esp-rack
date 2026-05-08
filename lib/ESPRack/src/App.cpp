#include "App.h"
#include "NullSecurityManager.h"
#include "ESPFS.h"
#include "Features.h"
#include "Version.h"
#include "WebFeatureSpec.h"
#include "TLSContextService.h"

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
    // 32 slot ceiling for AsyncWebSocket clients across all WS paths.
    // Originally 10, which got exhausted in two real-world scenarios:
    //   * operator opens 2-3 browser tabs against different live tabs
    //     (Light + Telegram + NTP) → 6+ active clients steady state;
    //   * useWs's stale-watchdog reconnect cycle leaks half-closed
    //     clients faster than beginPingPong can evict them (~80s).
    // Once the pool is full, ALL new WS handshakes fail until the
    // server drops zombies — which presented to the operator as
    // "WS lost Live and refresh doesn't help". 32 gives ~4× headroom
    // for the typical 4-5 concurrent live-tab use case; bumping
    // further is cheap (each slot is small, mostly a few pointers).
    wsManager_    {server, 32},
    // CRITICAL: pass our OWN deviceName_/deviceVersion_ buffers to
    // WebManager, not the ctor parameters. The caller's pointers
    // (typically Builder::deviceName_.c_str()) become dangling the
    // moment Builder goes out of scope — using them later was the
    // root cause of (a) garbled `device.name` in /rest/uiManifest
    // ("΂@" pattern), (b) corrupted JSON breaking the manifest fetch
    // and emptying the menu, and (c) multi_heap poison-pattern
    // crashes ~minutes into uptime as the freed bytes got reused by
    // unrelated allocations. Member-init order (deviceName_ before
    // webManager_, see App.h declaration order) guarantees the
    // String is fully constructed when we read its c_str() here.
    webManager_   {server,
                   &nullSecurity(),
                   &wsManager_,
                   deviceName_.c_str(),
                   deviceVersion_.c_str()},
    security_     {&nullSecurity()},
    // tlsContext_ + tls_ have to follow security_ in init order to
    // match the declaration sequence in App.h (telegram_/mqtt_ between
    // are pointer defaults from the header initialiser, no init-list
    // entry needed). C++ runs ctors in declaration order regardless
    // of init-list sequence; -Wreorder fires if these don't match.
    tlsContext_   {std::unique_ptr<TLSContextService>(new TLSContextService())},
    tls_          {tlsContext_.get()} {
  // Framework-owned UI shell — registered BEFORE Builder runs module
  // onInstall, so that modules calling addTabToFeature("system", ...)
  // find a parent. The buildFeatures map (exposed via /rest/uiManifest)
  // is populated later in App::begin() once modules_ is filled — keys
  // are derived from each installed Module's describe().id, so the
  // manifest reflects "what's installed" rather than a hardcoded
  // FT_*-driven list. The single non-module flag — FT_PROJECT (gates
  // the consumer's own service) — is registered there too as a
  // special case.

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

  // 0. Publish framework + module identity into the manifest so the
  // frontend (and audit tooling) can read the running rev set without
  // any out-of-band query. Each installed module contributes:
  //   * an entry in modules[] (id + version) — the module rev list
  //   * a buildFeatures[id] = true entry — the install map
  // Modules NOT installed via Builder().install<>() are simply absent
  // from both lists, so manifest.buildFeatures becomes a faithful
  // mirror of the .install<>() chain in main.cpp. No more drift
  // between FT_* macros and what the consumer actually wired up.
  //
  // FT_PROJECT remains a special case — it's a consumer-side toggle
  // (gates the consumer's own service body) and isn't tied to any
  // library Module. Registered explicitly so the frontend can still
  // hide consumer-specific UI pieces when the consumer ships a
  // framework-only build. Pointers come from describe() implementations,
  // by convention string literals embedded in module source — they live
  // in flash and outlive any request that reads them.
  webManager_.setFrameworkVersion(ESPRACK_VERSION_STR);
  webManager_.registerBuildFeature("project", FT_ENABLED(FT_PROJECT));
  for (auto& m : modules_) {
    if (!m) continue;
    ModuleDescriptor d;
    m->describe(d);
    webManager_.registerModule(d.id, d.version);
    webManager_.registerBuildFeature(d.id, true);
  }

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
