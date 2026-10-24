#include "App.h"
#include "NullSecurityManager.h"
#include "ESPFS.h"
#include "Features.h"
#include "Version.h"
#include "WebFeatureSpec.h"
#include "TLSContextService.h"
#include "DeviceIdentity.h"
#include "DataApi.h"
#include "HeapMonitor.h"
#include "ProblemJson.h"
#include "time/TimeSync.h"
#include "hw/ExtHwModule.h"
#include "hw/HwRegistry.h"
#include "system/BootGuard.h"
#include "system/CrashDump.h"
#include "system/LoopWdt.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_attr.h>
#include <esp_rom_crc.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <stddef.h>
#include <string.h>

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

// The running App, for restartDevice() / the esp shutdown hook (LIB-FIX-006).
App* s_app = nullptr;

// the reason restartDevice() was given, kept across the reset in
// RTC memory (not power loss). 32 bytes.
struct RestartRecord {
  uint32_t magic;
  char reason[24];
  uint32_t crc;
};
static_assert(sizeof(RestartRecord) == 32, "restart record layout");
constexpr uint32_t kRestartMagic = 0x31525352u;  // "RSR1"
RTC_NOINIT_ATTR RestartRecord s_restartRecord;

uint32_t restartRecordCrc(const RestartRecord& r) {
  return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&r), offsetof(RestartRecord, crc));
}

void storeRestartRecord(const char* reason) {
  RestartRecord r{};
  r.magic = kRestartMagic;
  strlcpy(r.reason, reason ? reason : "unknown", sizeof(r.reason));
  r.crc = restartRecordCrc(r);
  s_restartRecord = r;
}

char s_lastReason[24] = {0};

const char* hardwareReason(esp_reset_reason_t rr) {
  switch (rr) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "ext";
    case ESP_RST_SW:        return "idf";  // esp_restart() without restartDevice()
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int-wdt";
    case ESP_RST_TASK_WDT:  return "task-wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    default:                return "unknown";
  }
}

// Once per boot, before anything can call restartDevice() again.
void resolveLastRestartReason() {
  if (s_lastReason[0]) return;
  const esp_reset_reason_t rr = esp_reset_reason();
  const RestartRecord r = s_restartRecord;
  const bool valid = r.magic == kRestartMagic && r.crc == restartRecordCrc(r) &&
                     memchr(r.reason, '\0', sizeof(r.reason)) != nullptr && r.reason[0];
  if (rr == ESP_RST_SW && valid) {
    strlcpy(s_lastReason, r.reason, sizeof(s_lastReason));
  } else {
    strlcpy(s_lastReason, hardwareReason(rr), sizeof(s_lastReason));
  }
  s_restartRecord.magic = 0;  // used: a later non-SW reset never reuses it
}
}  // namespace

#ifndef ESPRACK_RESTART_DEADLINE_MS
// A restart resets the chip within this, whatever the hooks do (App.h).
#define ESPRACK_RESTART_DEADLINE_MS 30000
#endif

namespace {
// The restart deadline: one esp_timer, created in App::begin(), started by
// every restart path. Never stopped — the chip resets before it fires, or it
// resets the chip.
esp_timer_handle_t s_deadline = nullptr;

void deadlinePassed(void*) {
  log_e("[restart] deadline passed — a shutdown step hangs, resetting now");
  esp_restart();  // the hooks are already claimed: straight to the reset
}

void armRestartDeadline(uint32_t ms) {
  if (!s_deadline) return;
  (void)esp_timer_stop(s_deadline);  // ESP_ERR_INVALID_STATE when not running
  (void)esp_timer_start_once(s_deadline, (uint64_t)ms * 1000);
}
}  // namespace

#ifndef ESPRACK_SHUTDOWN_BUDGET_MS
// Per-module onShutdown() budget. Not preemptive (a hook can't be
// interrupted) — an overrun is logged so the offending module shows up.
#define ESPRACK_SHUTDOWN_BUDGET_MS 500
#endif

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
    // root cause of (a) garbled `device.name` in /api/v1/ui/manifest
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
    tls_          {tlsContext_.get()},
    hw_           {new HwRegistry()} {
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
  // find a parent. The buildFeatures map (exposed via /api/v1/ui/manifest)
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

  // Why this boot happened: read the RTC record now, before
  // anything can restart again.
  resolveLastRestartReason();
  log_i("[boot] last restart: %s", s_lastReason);

  s_app = this;
}

App::~App() {
  if (begun_) shutdown();
  if (s_app == this) s_app = nullptr;
}

void App::adoptModules(std::vector<std::unique_ptr<Module>>&& modules) {
  modules_ = std::move(modules);
}

Module* App::findModule(const char* moduleId) const {
  if (!moduleId || !*moduleId) return nullptr;
  for (const auto& m : modules_) {
    if (!m) continue;
    // An instance's id is its instance id, whatever describe() says.
    if (ExtHwModule* x = m->asExtHw()) {
      if (strcmp(x->instanceId(), moduleId) == 0) return m.get();
      continue;
    }
    ModuleDescriptor d;
    m->describe(d);
    if (d.id && strcmp(d.id, moduleId) == 0) return m.get();
  }
  return nullptr;
}

void App::begin() {
  if (begun_) return;
  begun_ = true;

  // Restart plumbing (LIB-FIX-006): remember the loop task (restartDevice
  // hands off-loop restarts to it) and hook esp_restart() itself so a
  // reboot that bypasses restartDevice (ArduinoOTA, Update, a library)
  // still runs the modules' onShutdown().
  loopTask_ = xTaskGetCurrentTaskHandle();
  if (esp_register_shutdown_handler(&App::onEspShutdown) != ESP_OK) {
    log_w("[restart] esp_register_shutdown_handler failed — only restartDevice() runs onShutdown");
  }
  if (!s_deadline) {
    const esp_timer_create_args_t args{&deadlinePassed, nullptr, ESP_TIMER_TASK, "restart-dl", false};
    if (esp_timer_create(&args, &s_deadline) != ESP_OK) {
      s_deadline = nullptr;
      log_w("[restart] no deadline timer — a hanging onShutdown() can keep the device down");
    }
  }

  // Time first: restores a soft-reset hold and owns the TZ
  // before any module looks at the clock. system.setTime is registered here,
  // i.e. before webManager_.begin() mounts the endpoints.
  TimeSync::instance().begin();
  TimeSync::instance().registerManifest(&webManager_);
  // Boot-loop guard: before any module starts, so onBegin()
  // can read BootGuard::info().safeMode. A consumer may call begin() with
  // other limits before app->begin(); the first call wins.
  BootGuard::begin();
  BootGuard::registerManifest(&webManager_);
  CrashDump::registerManifest(&webManager_);
  // System -> Hardware tab + "Restore factory pins" (LIB-FEAT-001).
  hw_->registerManifest(&webManager_);

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
  //
  // CRITICAL (config durability): mount WITHOUT format-on-fail first. The old
  // `ESPFS.begin(true)` reformatted the partition on ANY mount failure —
  // a transient hiccup or a LittleFS-version skew across a reflash would
  // silently wipe every config file AND every .snapshot backup (they live on
  // the same partition), which is exactly the "config gets overwritten when I
  // reflash" symptom. Now: try a clean mount, retry once, and only as a LAST
  // resort format a genuinely unmountable FS (first-ever boot / real
  // corruption) — and log loudly so a wipe is never silent.
  bool fsOk = ESPFS.begin(false);
  if (!fsOk) {
    log_e("[fs] LittleFS mount failed — retrying once before any format");
    delay(50);
    fsOk = ESPFS.begin(false);
  }
  if (!fsOk) {
    log_e("[fs] LittleFS still unmountable — FORMATTING (all config + "
          "snapshots on this partition will be lost; expected only on a "
          "first-ever boot or a genuinely corrupt filesystem)");
    fsOk = ESPFS.begin(true);  // format-on-fail: deliberate last resort only
    if (!fsOk) log_e("[fs] LittleFS format+mount FAILED — running without FS");
  }
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

  // 3.5 Hardware claims (LIB-FEAT-001): every external HW instance
  // describes its pins (configs can load now), first claimer wins. An
  // instance in Conflict leaves its hardware alone in onBegin/onLoop.
  hw_->boot();

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
  // /api/v1/ui/manifest. Must come AFTER all modules have registered.
  webManager_.begin();
  // The time zone is served by UI_DEVICE_PATH, so the manifest no longer
  // changes with it: one flash write per firmware.
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
        // index.html, "/" and the SPA fallback revalidate on every load
        // (no-cache + a content ETag): a cached index.html would point at
        // bundle files a firmware update removed. Hashed files never change.
        const char* cache_header =
            hashed_asset  ? "public, max-age=31536000, immutable"
          : static_image  ? "public, max-age=86400"
          :                 "no-cache";
        String etag;
        if (!hashed_asset) {
          char b[16];
          snprintf(b, sizeof(b), "\"%08x\"", (unsigned)esp_rom_crc32_le(0, content, len));
          etag = b;
        }
        const String uriCopy = uri;
        ArRequestHandlerFunction handler = [contentType, content, len, uriCopy, cache_header, etag](AsyncWebServerRequest* req) {
          if (etag.length() && req->hasHeader("If-None-Match") && req->header("If-None-Match") == etag) {
            AsyncWebServerResponse* nm = req->beginResponse(304, "text/plain", "");
            nm->addHeader("ETag", etag);
            nm->addHeader("Cache-Control", cache_header);
            req->send(nm);
            return;
          }
          AsyncWebServerResponse* res = req->beginResponse(200, contentType, content, len);
          res->addHeader("Content-Encoding", "gzip");
          res->addHeader("Cache-Control", cache_header);
          if (etag.length()) res->addHeader("ETag", etag);
          req->send(res);
        };
        srv->on(uri.c_str(), HTTP_GET, handler);
        if (uri.equals("/index.html")) {
          srv->onNotFound([handler](AsyncWebServerRequest* req) {
            if (req->method() == HTTP_OPTIONS) {
              req->send(200);                       // CORS preflight
            } else if (req->url().startsWith("/js/") || req->url().startsWith("/css/") ||
                       req->url().startsWith("/fonts/")) {
              req->send(404);                       // a bundle file this firmware doesn't have
            } else if (req->url().startsWith("/api/")) {
              // Unknown API path => clean 404, never the SPA shell. A module
              // that isn't installed mounts no handler, so its /api/* routes
              // must read as "gone", not "alive with HTML".
              sendProblem(req, 404, "not_found", "No such endpoint");
            } else if (req->method() == HTTP_GET) {
              handler(req);                         // SPA fallback (client routes)
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
    if (req->method() == HTTP_OPTIONS) {
      req->send(200);                       // CORS preflight
    } else if (req->url().startsWith("/api/")) {
      sendProblem(req, 404, "not_found", "No such endpoint");
    } else if (req->method() == HTTP_GET) {
      req->send(ESPFS, "/www/index.html");
    } else {
      req->send(404);
    }
  });
#endif

  // 7.5 — Per-connection service-latency guard. ESPAsyncWebServer arms
  // every accepted socket with rxTimeout=3 s (WebServer.cpp ctor) on top
  // of AsyncTCP's 5 s ack-timeout — tuned for a dual-core ESP32 where
  // async_tcp owns a full core. On single-core C3/C6 a first-load burst
  // (icons + manifest + REST forms, ~6 parallel) shares ONE core between
  // WiFi, lwIP, loopTask and async_tcp; per-connection service latency
  // can exceed those timers and the server KILLS ITS OWN healthy-but-
  // queued connections — the "Network Error" / broken-brand-icon symptom
  // (measured on C3: 17/60 failures at 6-way concurrency, 15/15
  // sequential). This middleware runs on every request right before
  // handler dispatch and relaxes both timers so slow-but-progressing
  // connections survive the burst. 10 s still reaps genuinely dead /
  // idle keep-alive sockets fast enough to protect the 16-slot lwIP PCB
  // pool, slow-loris protection is intact (the stock 3 s window keeps
  // applying until headers complete), and WebSocket / EventSource
  // upgrades are unaffected (their handlers zero rxTimeout afterwards).
#ifndef ESPRACK_HTTP_RX_TIMEOUT_S
#define ESPRACK_HTTP_RX_TIMEOUT_S 10
#endif
#ifndef ESPRACK_HTTP_ACK_TIMEOUT_MS
#define ESPRACK_HTTP_ACK_TIMEOUT_MS 10000
#endif
  server_->addMiddleware([](AsyncWebServerRequest* r, ArMiddlewareNext next) {
    AsyncClient* c = r->client();
    if (c) {
      c->setRxTimeout(ESPRACK_HTTP_RX_TIMEOUT_S);
      c->setAckTimeout(ESPRACK_HTTP_ACK_TIMEOUT_MS);
    }
    next();
  });

  // 7.6 — C5 data API: /api/v1/data[/<series>[/latest]]. Mounted here
  // (after module install, so security_ is the real manager; before
  // server begin) so any series registered during onInstall/onBegin is
  // already in the registry. Reader-token consumable — GET only.
  DataApi::instance().mount(server_, security_);

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

  // Time first: SNTP / operator samples, clock events.
  TimeSync::instance().loop();

  // Config saves PowerGuard paused, once writes are allowed again.
  configManager_.flushDeferred();

  // A restart requested from another task is performed HERE, on the loop
  // task, so onShutdown() never runs concurrently with the same module's
  // onLoop() (LIB-FIX-006).
  if (restartRequested_.load()) {
    finishAndRestart();
  }

  hw_->loop();  // a pending "Restore factory pins" (restarts)
  BootGuard::loop();  // markStable() once the uptime reaches stableAfterMs

  // Module loop fan-out. The per-module profiler and the heap-drift
  // sampler that bracket it are DEBUG instrumentation (FT_LOOP_PROFILE /
  // FT_HEAP_MONITOR), built for the perf / fragmentation hunt and
  // compiled out of shipping builds — leaving just the bare onLoop fan-
  // out below. Flip the flags in features.ini to bring either back.
#if FT_ENABLED(FT_LOOP_PROFILE)
  // Once every 60 s, snapshot per module:
  //   * loopTask stack high-water BEFORE/AFTER m->onLoop() — who sinks stack
  //   * free heap / max_alloc deltas — who churns heap inside its tick
  //   * elapsed micros — who spends the most CPU
  // Off-cycle passes skip instrumentation entirely (one millis() compare).
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
#else
  for (auto& m : modules_) {
    if (m) m->onLoop();
  }
#endif  // FT_LOOP_PROFILE
  wsManager_.processAllQueues();

#if FT_ENABLED(FT_HEAP_MONITOR)
  // Heap drift sampler — 60 s cadence, zero-alloc. Captures the slow
  // fragmentation slope that predicts TLS-handshake failure long before
  // the failure happens. See HeapMonitor.h for the uptime rationale.
  static uint32_t s_lastHeapTick_ms = 0;
  static uint8_t  s_ticksSinceLog  = 0;
  const uint32_t hb_now = millis();
  if (hb_now - s_lastHeapTick_ms >= 60000) {
    s_lastHeapTick_ms = hb_now;
    HeapMonitor::tick();
    // Periodic visibility: print every 5 ticks (≈5 min) — enough to spot
    // drift over the hours-to-days timescale that matters for uptime.
    if (++s_ticksSinceLog >= 5) {
      s_ticksSinceLog = 0;
      HeapMonitor::logSnapshot("soak");
    }
  }
#endif  // FT_HEAP_MONITOR
}

void App::shutdown() {
  runShutdownOnce("shutdown");
}

bool App::runShutdownOnce(const char* reason) {
  if (shutdownDone_.exchange(true)) return false;   // another path owns the hooks
  if (!begun_) {
    hooksDone_ = true;
    return true;
  }
  begun_ = false;                             // App::loop stops ticking modules
  // The hooks may take longer than the loop watchdog in total; the restart
  // deadline (restartDevice) bounds them instead.
  LoopWdt::pause();
  log_w("[restart] graceful shutdown (reason=%s)", reason ? reason : "?");
  // Reverse install order so dependents tear down first.
  for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
    if (!*it) continue;
    if (abandonHooks_.load()) {               // the caller gave up on us and resets
      log_w("[restart] remaining onShutdown hooks skipped");
      break;
    }
    const uint32_t t0 = millis();
    (*it)->onShutdown();
    const uint32_t dt = millis() - t0;
    if (dt > ESPRACK_SHUTDOWN_BUDGET_MS) {
      ModuleDescriptor d;
      (*it)->describe(d);
      log_w("[restart] '%s' onShutdown took %u ms (budget %u ms)",
            d.id ? d.id : "?", (unsigned)dt, (unsigned)ESPRACK_SHUTDOWN_BUDGET_MS);
    }
  }
  // The clock keeps running across a soft reset: note that it was valid
  // until now, so the next boot can hold it (RtcHold).
  TimeSync::instance().flushHold();
  hooksDone_ = true;
  return true;
}

uint32_t App::shutdownGraceMs() const {
  return (uint32_t)modules_.size() * ESPRACK_SHUTDOWN_BUDGET_MS + 2000;
}

uint32_t App::restartDeadlineMs() const {
  // Longer than an off-loop caller's own patience (3 s handoff + grace), so
  // its orderly takeover (abandon the hooks, still run afterShutdown) wins.
  const uint32_t need = 3000 + shutdownGraceMs() + 5000;
  return need > ESPRACK_RESTART_DEADLINE_MS ? need : ESPRACK_RESTART_DEADLINE_MS;
}

bool App::waitHooks(uint32_t ms) const {
  const uint32_t t0 = millis();
  while (!hooksDone_.load() && millis() - t0 < ms) vTaskDelay(pdMS_TO_TICKS(20));
  return hooksDone_.load();
}

void App::runAfterShutdownOnce() {
  uint8_t expected = 0;
  if (afterState_.compare_exchange_strong(expected, 1)) {
    if (afterShutdown_) {
      armRestartDeadline(ESPRACK_RESTART_DEADLINE_MS);  // its own allowance
      afterShutdown_();
    }
    afterState_ = 2;
    return;
  }
  // Another task is inside afterShutdown (e.g. wiping /config): don't reset
  // under it. Bounded, so a hang can't keep the device from rebooting.
  const uint32_t t0 = millis();
  while (afterState_.load() != 2 && millis() - t0 < 5000) vTaskDelay(pdMS_TO_TICKS(20));
}

void App::finishAndRestart() {
  if (!runShutdownOnce(restartReason_) && !waitHooks(shutdownGraceMs())) {
    // Another task owns the hooks and they overran: stop them between hooks
    // and finish here.
    abandonHooks_ = true;
  }
  runAfterShutdownOnce();
  esp_restart();
}

void App::onEspShutdown() {
  // esp_restart() outside restartDevice(): the hooks run here, in that task.
  if (s_app && !s_app->shutdownDone_.load()) {
    armRestartDeadline(s_app->restartDeadlineMs());
    s_app->runShutdownOnce("idf");
  }
}

const char* restartReason() {
  return (s_app && s_app->shutdownDone_.load()) ? s_app->restartReason_ : "";
}

const char* lastRestartReason() {
  resolveLastRestartReason();
  return s_lastReason;
}

void restartDevice(const char* reason, std::function<void()> afterShutdown) {
  App* a = s_app;
  resolveLastRestartReason();                 // read this boot's record before overwriting it
  if (!a) {                                   // no framework: nothing to shut down
    storeRestartRecord(reason);
    if (afterShutdown) afterShutdown();
    esp_restart();
  }

  const bool onLoop =
      a->loopTask_ && xTaskGetCurrentTaskHandle() == static_cast<TaskHandle_t>(a->loopTask_);
  // Off the loop task this caller parks for seconds (handoff + hook budget).
  // async_tcp — where the restart/factory-reset/restore handlers run — is
  // subscribed to the task watchdog (panic): unsubscribe, the device reboots
  // anyway (the restart deadline below guarantees it). Harmless (ESP_ERR_NOT_FOUND) for unsubscribed tasks.
  if (!onLoop) (void)esp_task_wdt_delete(nullptr);

  // First caller wins. A later caller (any task) just waits for the reset
  // already under way — it must not cut the running hooks short.
  bool expected = false;
  if (!a->restartClaimed_.compare_exchange_strong(expected, true)) {
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
  }

  a->restartReason_ = reason ? reason : "unknown";
  a->afterShutdown_ = std::move(afterShutdown);
  // Survives the reset: lastRestartReason() of the next boot.
  // Written before the hooks, so a hook that hangs doesn't lose it.
  storeRestartRecord(a->restartReason_);
  armRestartDeadline(a->restartDeadlineMs());

  if (!a->begun_ || onLoop) a->finishAndRestart();

  // Off the loop task: hand off to App::loop(), which runs the hooks there
  // (never concurrently with a module's onLoop) and resets the chip.
  a->restartRequested_ = true;
  uint32_t t0 = millis();
  while (!a->shutdownDone_.load() && millis() - t0 < 3000) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (a->shutdownDone_.load()) {
    // The loop is running the hooks — give them their budget. Normally the
    // loop finishes, runs afterShutdown and resets the chip while we wait.
    if (!a->waitHooks(a->shutdownGraceMs())) {
      // A hook is stuck: stop the rest and finish here, so e.g. a factory
      // reset still wipes /config before the reset the UI already reported.
      log_w("[restart] shutdown hooks exceeded %u ms — finishing from the caller task",
            (unsigned)a->shutdownGraceMs());
      a->abandonHooks_ = true;
    }
    a->runAfterShutdownOnce();   // or wait for the loop that is running it
    esp_restart();
  }
  log_w("[restart] loop did not respond within 3 s — shutting down from the caller task");
  a->finishAndRestart();
}

}  // namespace ESPRack
