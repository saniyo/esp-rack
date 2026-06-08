#include "App.h"
#include "NullSecurityManager.h"
#include "ESPFS.h"
#include "Features.h"
#include "Version.h"
#include "WebFeatureSpec.h"
#include "TLSContextService.h"
#include "DeviceIdentity.h"
#include "HeapMonitor.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

// Firmware-tag anchors — keep the strings emitted in FwTags.cpp
// alive through --gc-sections. A function-local volatile load isn't
// enough (compiler drops the load as a dead store before the linker
// even sees a use), so we read each through Serial.printf below —
// the printf's external side effect anchors the symbol references
// the linker needs to keep .rodata alive into firmware.bin where
// the update-server's regex grep finds them.
extern "C" {
extern const char ESPRACK_TAG_BASE[];
extern const char ESPRACK_TAG_FLV[];
extern const char ESPRACK_TAG_VER[];
}

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
    // 16 slot ceiling for AsyncWebSocket clients across all WS paths.
    // Originally 10 → bumped to 32 to absorb the useWs stale-watchdog
    // reconnect leak. Then the heap audit (see docs/plans/memory-
    // fragmentation-master-plan.md, "AsyncWebSocket pool" section)
    // showed those 32 slots cost ~8 KB of scattered allocations that
    // were drawing the post-setup max-contiguous below the mbedtls
    // 32 KB ask. 16 keeps ~3× headroom over the realistic 4-5 tab
    // concurrent-live use case while halving the heap residency of
    // the WS pool. If you raise this back, also raise the
    // beginPingPong eviction cadence so zombies evict faster.
    wsManager_    {server, 16},
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
  // Publish the canonical identity strings to DeviceIdentity so every
  // consumer reads the same source of truth — CertManagerService
  // (Subject CN), AutoUpdateService (HTTP headers / URL query),
  // MothershipService (checkin payload), SystemStatus (Identity tab).
  // On Arduino-ESP32 esp_app_get_description() leaks "arduino-lib-
  // builder" through, so we override with what Builder("...","...")
  // got and what the consumer's factory_settings.ini declared.
  DeviceIdentity::setProjectName(deviceName_.c_str());
  DeviceIdentity::setVersion(deviceVersion_.c_str());
#ifdef FACTORY_HW_REVISION
  DeviceIdentity::setHwRevision(FACTORY_HW_REVISION);
#endif

  // Anchor firmware tags by reading them through Serial.printf (the
  // compiler can't elide the printf — it has external side effects),
  // which gives the linker undefined references to each TAG symbol
  // that --gc-sections can't drop. Doubles as a useful boot-log line
  // identifying which project/flavor/version is running.
  Serial.printf("[fw] %s | %s | %s\n",
                ESPRACK_TAG_BASE, ESPRACK_TAG_FLV, ESPRACK_TAG_VER);

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

  // Anchor the fragmentation-watchdog reference. tick() in loop()
  // will append per-minute samples from here forward; this first
  // call captures "what does FRESH heap look like" so the slope
  // calculation has a baseline to grow from. We DON'T logSnapshot
  // here — the per-checkin "post-warmup" anchor and the every-5-min
  // "soak" snapshot give enough visibility without an extra boot
  // line.
  ESPRack::HeapMonitor::tick();

  // TLS heap-OOM (MBEDTLS_ERR_SSL_ALLOC_FAILED -32512) — fixed for
  // good at the framework-rebuild level in
  // esp-rack-light-demo/platformio.ini via `custom_sdkconfig` that
  // shrinks `CONFIG_MBEDTLS_SSL_IN/OUT_CONTENT_LEN` from 16 KB each
  // to 4 KB each. mbedtls record buffers now fit a per-handshake
  // budget of ~10 KB instead of ~32 KB, comfortably inside any
  // plausible ESP32-C3 max-contiguous-alloc window even after hours
  // of heap-fragmenting traffic.
  //
  // Earlier attempts that DIDN'T work — kept in the audit log:
  //   * MbedtlsArena (custom allocator + mbedtls hooks) corrupted
  //     parsed CA chain mid-verify (-9984 X509_CERT_VERIFY_FAILED).
  //   * TlsHeapReserve (heap_caps_malloc 40 KB + Lease release-
  //     reacquire pattern) shrank system free-heap by 40 KB at boot
  //     and the released block didn't flow back to where mbedtls
  //     allocates from — handshake hit MBEDTLS_ERR_SSL_ALLOC_FAILED
  //     on EVERY attempt at uptime 82 s, worse than no fix at all.
  //   * Soft-watchdog (esp_restart after N consecutive fails) was
  //     a workaround unsuitable for a certified-system uptime
  //     target measured in years; rejected on review.

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

  // Phase-by-phase heap instrumentation. Each `boot:<tag>` line lets
  // us pinpoint which startup step is eating contiguous heap. Pure
  // diagnostic — these calls only read ESP heap APIs, no allocations.
  HeapMonitor::logSnapshot("boot:enter");

  // 1. Filesystem first — ConfigManager and several modules need /config
  // available before they can ensureLoaded().
  ESPFS.begin(true);
  HeapMonitor::logSnapshot("boot:fs-mounted");

  // 2. ConfigManager picks up its work directories under /config.
  configManager_.begin();
  HeapMonitor::logSnapshot("boot:cfgmgr-begun");

  // 3. Bring up lwIP early — modules that touch UDP/TCP in onBegin
  // (NTP, OTA, Telegram) crash on Arduino-3.x / IDF 5 if WiFi.mode
  // hasn't run yet. AP_STA performs the heavy "Wi-Fi driver +
  // network interfaces" init in one shot regardless of which
  // module(s) actually use STA vs AP.
  WiFi.mode(WIFI_AP_STA);
  HeapMonitor::logSnapshot("boot:wifi-mode-set");

  // 4. Modules' onInstall has already happened in Builder.build();
  // here we run their onBegin in the same install order. Per-module
  // heap snapshot — if any single module's onBegin punches a big hole
  // (8 KB+ contiguous loss), we'll see it as a step-change in the
  // post-<id> line.
  for (auto& m : modules_) {
    if (m) {
      ModuleDescriptor d;
      m->describe(d);
      m->onBegin();
      log_i("[boot] post-%s: free=%u max_alloc=%u",
            d.id ? d.id : "?",
            (unsigned)ESP.getFreeHeap(),
            (unsigned)ESP.getMaxAllocHeap());
    }
  }
  HeapMonitor::logSnapshot("boot:modules-begun");

  // 5. WebManager.begin() iterates registered features and mounts
  // /rest/uiManifest. Must come AFTER all modules have registered.
  webManager_.begin();
  HeapMonitor::logSnapshot("boot:webmanager-begun");

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
        // Per-asset Cache-Control. Hashed bundle assets (/js/*.HASH.js,
        // /css/*.HASH.css, /fonts/*.woff2) are content-addressed by
        // the build pipeline and never change for a given firmware —
        // safe to tell the browser to cache them forever. Everything
        // else (/, /index.html, /favicon.ico, /app/manifest.json) can
        // change on a UI iteration without a new firmware, so cache
        // for a few minutes only — long enough to stop the browser's
        // favicon-refetch-every-second storm but short enough that a
        // hot-flashed bundle still shows up on the next reload.
        // Tier 1 — content-hashed bundle assets (/js/*.HASH.js,
        // /css/*.HASH.css, /fonts/*.woff2): never change for a given
        // build, cache forever.
        bool hashed_asset = uri.startsWith("/js/")
                          || uri.startsWith("/css/")
                          || uri.startsWith("/fonts/");
        // Tier 2 — static images baked into the firmware (brand
        // /app/icon.png, /favicon.ico, any /app/*.svg): they change only
        // on a reflash, NOT on a UI hot-iteration. The previous 10-min
        // policy made the browser RE-FETCH the brand icon on every page
        // load past that window; on the no-PSRAM C3 that re-fetch lands
        // in the concurrent-request failure window (verified: 1/40 ok
        // under 4-way concurrency, 15/15 sequential) and renders a BROKEN
        // ICON — the "sometimes the logo is missing" symptom. Cache them
        // for a day so the logo/favicon drop out of the per-load fetch
        // cascade entirely. A stale logo after a reflash is purely
        // cosmetic and self-heals within 24 h.
        bool static_image = uri.endsWith(".png")
                          || uri.endsWith(".ico")
                          || uri.endsWith(".svg")
                          || uri.endsWith(".webp");
        const char* cache_header =
            hashed_asset  ? "public, max-age=31536000, immutable"
          : static_image  ? "public, max-age=86400"
          :                 "public, max-age=600";
        const String uriCopy = uri;
        ArRequestHandlerFunction handler = [contentType, content, len, uriCopy, cache_header](AsyncWebServerRequest* req) {
          AsyncWebServerResponse* res = req->beginResponse(200, contentType, content, len);
          res->addHeader("Content-Encoding", "gzip");
          res->addHeader("Cache-Control", cache_header);
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
  HeapMonitor::logSnapshot("boot:server-started");
}

void App::loop() {
  if (!begun_) return;

  // Per-module loop profiler. Once every 60 s, snapshot:
  //   * stack high-water mark for loopTask BEFORE/AFTER each
  //     m->onLoop() — reveals which module sinks the most stack
  //   * free heap / max_alloc deltas — reveals which module churns
  //     heap inside its loop tick
  //   * elapsed micros — reveals which module spends the most CPU
  // Off-cycle ticks (every other 60 s pass) skip the instrumentation
  // entirely so the steady-state cost is one `millis()` compare per
  // loop iteration. Pure diagnostic — strip the block when the
  // per-module audit is done.
  static uint32_t s_lastProfile_ms = 0;
  const uint32_t now_ms = millis();
  const bool profile_this_pass = (now_ms - s_lastProfile_ms) >= 60000;
  if (profile_this_pass) {
    s_lastProfile_ms = now_ms;
    uint32_t stack_free_in  = uxTaskGetStackHighWaterMark(nullptr);
    log_i("[loop.profile] sample @ uptime=%us — loopTask stack_free_min=%u B",
          (unsigned)(now_ms / 1000), (unsigned)stack_free_in);
    for (auto& m : modules_) {
      if (!m) continue;
      ModuleDescriptor d;
      m->describe(d);
      uint32_t  free_in  = ESP.getFreeHeap();
      uint32_t  ma_in    = ESP.getMaxAllocHeap();
      uint32_t  stack_in = uxTaskGetStackHighWaterMark(nullptr);
      uint64_t  t0       = esp_timer_get_time();
      m->onLoop();
      uint64_t  t1       = esp_timer_get_time();
      uint32_t  free_out = ESP.getFreeHeap();
      uint32_t  ma_out   = ESP.getMaxAllocHeap();
      uint32_t  stack_out= uxTaskGetStackHighWaterMark(nullptr);
      int32_t dheap  = (int32_t)free_out - (int32_t)free_in;
      int32_t dma    = (int32_t)ma_out   - (int32_t)ma_in;
      int32_t dstack = (int32_t)stack_out- (int32_t)stack_in;  // negative => stack consumed
      log_i("[loop.profile]   %-16s dt=%lluus dheap=%d dmax=%d dstack=%d",
            d.id ? d.id : "?",
            (unsigned long long)(t1 - t0),
            (int)dheap, (int)dma, (int)dstack);
    }
  } else {
    for (auto& m : modules_) {
      if (m) m->onLoop();
    }
  }
  wsManager_.processAllQueues();

  // Heap drift sampler — 60 s cadence, zero-alloc. Captures the slow
  // fragmentation slope that predicts TLS-handshake failure long
  // before the failure actually happens. See HeapMonitor.h for the
  // certified-uptime rationale.
  static uint32_t s_lastHeapTick_ms = 0;
  static uint8_t  s_ticksSinceLog  = 0;
  if (now_ms - s_lastHeapTick_ms >= 60000) {
    s_lastHeapTick_ms = now_ms;
    HeapMonitor::tick();
    // Periodic visibility: print every 5 ticks (≈5 min). Cheap on
    // serial bandwidth, but enough resolution to spot a drift over
    // the hours-to-days timescale that matters for certified uptime.
    if (++s_ticksSinceLog >= 5) {
      s_ticksSinceLog = 0;
      HeapMonitor::logSnapshot("soak");
    }
  }
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
