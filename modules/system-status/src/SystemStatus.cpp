#include <SystemStatus.h>

#include <AdaptiveJson.h>
#include <FormBuilder.h>   // still used by buildIdentityForm during migration
#include <web/Form.h>
#include <WebManager.h>
#include <DeviceIdentity.h>
#include <HeapMonitor.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Mothership is an optional sibling module: read it through the core
// interface (App::mothership()), never its header. An `#if
// __has_include(<MothershipService.h>)` here made PlatformIO's LDF pull the
// whole mothership module into every build that used system-status
// (LIB-FIX-012). Without mothership the "Heap @ last checkin" row is skipped.
#include <App.h>
#include <IMothershipProvider.h>

SystemStatus::SystemStatus(AsyncWebServer* server, SecurityManager* securityManager, WebManager* web)
    : _server(server), _sec(securityManager), _web(web) {
  // Endpoint binding moved into registerManifest below so the routes
  // flow through WebManager::registerPage instead of raw server->on.
  // Phase 5/Phase 4 follow-up: rest.proxy can now reach
  // SYSTEM_STATUS_FORM_PATH / SYSTEM_IDENTITY_FORM_PATH from Mothership
  // without HTTP loopback.
  (void)_server; (void)_sec;
}

void SystemStatus::registerManifest(WebManager* web) {
  if (!web) return;
  using namespace std::placeholders;

  // Endpoint registry — same paths, same handlers, just routed via
  // WebManager so the rest.proxy in-process dispatcher reaches them.
  WebPageSpec page;
  page.id        = "systemStatus";
  page.title     = "System Status";
  page.component = "";   // no top-level page route — contributes tabs
                          // to the compound 'system' feature below.
  page.auth      = WebAuthLevel::Authenticated;
  {
    WebEndpointMeta e;
    e.method = "GET"; e.path = SYSTEM_STATUS_SERVICE_PATH; e.role = "legacy_status";
    e.auth   = WebAuthLevel::Authenticated;
    e.handler = std::bind(&SystemStatus::systemStatus, this, _1);
    e.internalHandler =
      [this](JsonVariant /*in*/, JsonVariant out, int& status) {
        JsonObject o = out.to<JsonObject>();
        // legacy systemStatus emits hardware / heap fields directly.
        // Reuse buildForm by writing into our scratch object then
        // pulling the "status" sub-object out — but buildForm needs
        // a JsonObject root and we already have one. Slight contortion:
        DynamicJsonDocument scratch(4096);
        JsonObject root = scratch.to<JsonObject>();
        buildForm(root);
        JsonObject sect = root["status"].as<JsonObject>();
        if (!sect.isNull()) { o.set(sect); status = 200; }
        else { o["error"] = "buildForm produced empty status"; status = 500; }
      };
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "GET"; e.path = SYSTEM_STATUS_FORM_PATH; e.role = "resourcesForm";
    e.auth   = WebAuthLevel::Authenticated;
    e.handler = std::bind(&SystemStatus::systemStatusForm, this, _1);
    e.internalHandler =
      [this](JsonVariant /*in*/, JsonVariant out, int& status) {
        JsonObject o = out.to<JsonObject>();
        buildForm(o);
        status = 200;
      };
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "GET"; e.path = SYSTEM_IDENTITY_FORM_PATH; e.role = "identityForm";
    e.auth   = WebAuthLevel::Authenticated;
    e.handler = std::bind(&SystemStatus::systemIdentityForm, this, _1);
    e.internalHandler =
      [this](JsonVariant /*in*/, JsonVariant out, int& status) {
        JsonObject o = out.to<JsonObject>();
        buildIdentityForm(o);
        status = 200;
      };
    page.endpoints.push_back(std::move(e));
  }
  web->registerPage(std::move(page));

  // Identity tab — first. "Who is this device" comes before "how is
  // it doing". Holds canonical device ID, project, fw / framework
  // versions, chip / SDK, MAC, eFuse hardware UID, flash UID.
  WebTabSpec idtab;
  idtab.key       = "identity";
  idtab.title     = "Identity";
  idtab.restPath  = SYSTEM_IDENTITY_FORM_PATH;
  idtab.postable  = false;
  idtab.auth      = WebAuthLevel::Authenticated;
  idtab.order     = 10;
  web->addTabToFeature("system", idtab);

  // Resources tab — second. Pure runtime telemetry: CPU freq, heap,
  // PSRAM, sketch / flash / fs occupancy, installed modules. Same
  // restart / factory-reset actions hang off the bottom — they're
  // operations on the resources, not on the identity.
  WebTabSpec tab;
  tab.key = "status";  // legacy key kept; only the title flipped to "Resources"
  tab.title = "Resources";
  tab.restPath = SYSTEM_STATUS_FORM_PATH;
  tab.postable = false;
  tab.auth = WebAuthLevel::Authenticated;
  tab.order = 12;
  web->addTabToFeature("system", tab);
}

void SystemStatus::buildForm(JsonObject& root,
                              uint32_t free_snap,
                              uint32_t max_snap) {
  web::Form sta(root, "status", "Resources");

  // Pure runtime telemetry: CPU / heap / PSRAM / sketch / flash / fs.
  // Identity-class fields (device id, MAC, chip model, SDK, framework
  // version, module list) live in the Identity tab — see
  // buildIdentityForm() / SYSTEM_IDENTITY_FORM_PATH. String() wrapping
  // inside addTextField forces ArduinoJson to copy the bytes, so local
  // String lifetimes are safe.

  String cpuFreq = String(ESP.getCpuFreqMHz()) + " MHz";
  sta.add(web::Text("cpu_frequency", cpuFreq.c_str()).readOnly()
              .label("CPU frequency").icon("ShowChart"));

  // ── Three heap views, each captured at a different observer cost ──
  //
  // The form-GET measurement path inherently consumes ~3-5 KB while
  // running (AsyncJsonResponse's 2 KB DynamicJsonDocument pool +
  // lwIP TCP_PCB for the active HTTP session + AsyncRequest/Response
  // structs). A naive ESP.getFreeHeap() inside this function would
  // ALWAYS read ~5 KB below what the mothership task sees, which is
  // exactly the "розбіжність" the operator hits when comparing this
  // tab to the server-side device page. We expose all three sources
  // so the source of any discrepancy is visible at a glance:
  //
  //   1. "live, at request entry"  — captured by the route handler
  //      BEFORE allocating any response buffer. Closest to "what the
  //      heap was when this HTTP request arrived". Falls back to a
  //      live read if the caller didn't pass a snapshot.
  //
  //   2. "@ idle (last sample)"    — HeapMonitor's 60s-cadence ring,
  //      sampled from App::loop() between requests. Zero observer
  //      overhead — closest to "true baseline heap right now".
  //
  //   3. "@ last checkin"          — the exact value the mothership
  //      task sent to the server in the most recent /checkin body.
  //      MIRRORS the server-side device-panel header card; if these
  //      two ever diverge, it's a server-side cache/lag bug, not a
  //      device measurement bug.

  uint32_t fh_live = free_snap ? free_snap : ESP.getFreeHeap();
  uint32_t ma_live = max_snap  ? max_snap  : ESP.getMaxAllocHeap();
  String heap_live = String(fh_live) + " / " + String(ma_live) + " bytes";
  sta.add(web::Text("heap_free_max_alloc", heap_live.c_str()).readOnly()
              .label("Heap (live, at request entry)").icon("Memory"));

  // Idle baseline — HeapMonitor::snapshot() returns the most recent
  // ring sample plus a synthesised "live" fallback if the ring is
  // still empty (boot before first tick). uptime_s of the sample
  // lets the operator gauge how stale it is (cadence is 60 s; first
  // sample lands ~60 s after boot).
  {
    ESPRack::HeapSnapshot snap = ESPRack::HeapMonitor::snapshot();
    uint32_t age_s = 0;
    uint32_t now_s = (uint32_t)(millis() / 1000);
    if (snap.latest.uptime_s > 0 && now_s >= snap.latest.uptime_s) {
      age_s = now_s - snap.latest.uptime_s;
    }
    String heap_idle = String(snap.latest.free_bytes) + " / "
                     + String(snap.latest.max_alloc) + " bytes  ("
                     + String(age_s) + "s ago, "
                     + String(snap.sample_count) + " samples)";
    sta.add(web::Text("heap_at_idle", heap_idle.c_str()).readOnly()
                .label("Heap @ idle (last 60s tick)").icon("Memory"));
  }

  // Mirror of what mothership last shipped. Pre-checkin (or mothership
  // not installed) there is nothing to show and the row is skipped.
  {
    IMothershipProvider* ms = _app ? _app->mothership() : nullptr;
    uint32_t fh_ck = 0, ma_ck = 0, age_ms = 0;
    if (ms && ms->heapAtLastCheckin(fh_ck, ma_ck, age_ms)) {
      uint32_t age_s = age_ms / 1000;
      String heap_ck = String(fh_ck) + " / " + String(ma_ck) + " bytes  ("
                     + String(age_s) + "s ago)";
      sta.add(web::Text("heap_at_checkin", heap_ck.c_str()).readOnly()
                  .label("Heap @ last checkin (server mirror)").icon("CloudSync"));
    }
  }

  if (ESP.getPsramSize() > 0) {
    String psram = String(ESP.getPsramSize()) + " / " + String(ESP.getFreePsram()) + " bytes";
    sta.add(web::Text("psram_size_free", psram.c_str()).readOnly()
                .label("PSRAM (size / free)").icon("Apps"));
  }

  String sketch = String(ESP.getSketchSize()) + " / " + String(ESP.getFreeSketchSpace()) + " bytes";
  sta.add(web::Text("sketch_size_free", sketch.c_str()).readOnly()
              .label("Sketch (size / free)").icon("DataUsage"));

  String flash = String(ESP.getFlashChipSize()) + " bytes / "
               + String((int)(ESP.getFlashChipSpeed() / 1000000)) + " MHz";
  sta.add(web::Text("flash_chip_size_speed", flash.c_str()).readOnly()
              .label("Flash (size / speed)").icon("SdStorage"));

  size_t fsTotal = ESPFS.totalBytes();
  size_t fsUsed  = ESPFS.usedBytes();
  size_t fsFree  = fsTotal > fsUsed ? fsTotal - fsUsed : 0;
  String fs = String(fsUsed) + " / " + String(fsTotal) + " bytes (" + String(fsFree) + " free)";
  sta.add(web::Text("file_system_used_total", fs.c_str()).readOnly()
              .label("File system (used / total)").icon("Folder"));

  // ─── Per-task stack high-water marks ───────────────────────────
  // FreeRTOS stack high-water mark = MINIMUM free bytes ever seen on
  // that task's stack. Lets us right-size oversized stacks (the
  // Mothership 8 KB + Telegram 6 KB are prime candidates — actual
  // usage on a clean device is usually a small fraction). Trim
  // strategy: stack_size = high_water_usage × 1.5, rounded up to
  // 256 B. Each saved KB of stack goes straight back to the heap
  // pool — that's where we want it given the C3's tight budget.
  //
  // Rendered as a proper MUI DataGrid via addTableField — frontend
  // already knows how to display per-row data (same pattern as the
  // Config Manager and mDNS browse tabs).
  {
    const UBaseType_t n = uxTaskGetNumberOfTasks();
    if (n > 0) {
      TaskStatus_t* tasks = static_cast<TaskStatus_t*>(
          pvPortMalloc(n * sizeof(TaskStatus_t)));
      if (tasks) {
        const UBaseType_t got = uxTaskGetSystemState(tasks, n, nullptr);
        sta.add(web::Table("rtos_tasks")
                    .col("name", "Task", "text")
                    .col("prio", "Priority", "number", "0")
                    .col("stack_free", "Stack free (B)", "number", "0")
                    .label("FreeRTOS tasks").icon("Layers"));
        JsonArray rows = sta.rowsOf("rtos_tasks");
        for (UBaseType_t i = 0; i < got; i++) {
          JsonObject r = rows.createNestedObject();
          r["name"]       = tasks[i].pcTaskName;
          r["prio"]       = (uint32_t)tasks[i].uxCurrentPriority;
          r["stack_free"] = (uint32_t)tasks[i].usStackHighWaterMark;
        }
        vPortFree(tasks);
      }
    }
  }

  // Module table moved to the Identity tab — what's installed is part
  // of "who this device is" (firmware composition fingerprint), not
  // resource utilisation. See buildIdentityForm().

  sta.add(web::Action("a_restart").ref("system.restart"));
  sta.add(web::Action("a_factoryReset").ref("system.factoryReset"));
}

void SystemStatus::systemStatus(AsyncWebServerRequest* request) {
  // Snapshot heap BEFORE sendAdaptiveJson allocates the response doc.
  // See buildForm()'s comment block for the observer-effect rationale —
  // same principle: allocation of the response pool would otherwise eat
  // into the very value we're about to report.
  const uint32_t fh_entry = ESP.getFreeHeap();
  const uint32_t ma_entry = ESP.getMaxAllocHeap();
  sendAdaptiveJson(request, [fh_entry, ma_entry](JsonObject& root) {
    root["esp_platform"]    = ESP.getChipModel();
    root["max_alloc_heap"]  = ma_entry;
    root["psram_size"]      = ESP.getPsramSize();
    root["free_psram"]      = ESP.getFreePsram();
    root["cpu_freq_mhz"]    = ESP.getCpuFreqMHz();
    root["free_heap"]       = fh_entry;
    root["sketch_size"]     = ESP.getSketchSize();
    root["free_sketch_space"] = ESP.getFreeSketchSpace();
    root["sdk_version"]     = ESP.getSdkVersion();
    root["flash_chip_size"] = ESP.getFlashChipSize();
    root["flash_chip_speed"] = ESP.getFlashChipSpeed();

    // Normalized HW identification fields
    {
      String chip = ESP.getChipModel();
      chip.toLowerCase();
      chip.replace("-", "");
      chip.replace(" ", "");
      root["chip_id"] = chip;

      int flashMB = (int)((ESP.getFlashChipSize() + 512 * 1024) / (1024 * 1024));
      root["flash_mb"] = flashMB;

      int psramMB = 0;
      size_t psramSize = ESP.getPsramSize();
      if (psramSize > 0) {
        psramMB = (int)((psramSize + 512 * 1024) / (1024 * 1024));
      }
      root["psram_mb"] = psramMB;

      String suffix = chip + "-n" + String(flashMB);
      if (psramMB > 0) {
        suffix += "r" + String(psramMB);
      }
      root["hw_suffix"] = suffix;
    }

    // Canonical device identity — same source as cert Subject CN and
    // mothership deviceId payload, so monitoring tooling that scrapes
    // /api/v1/system/health can correlate this device with its server-
    // side record without parsing the cert.
    root["device_id"]    = DeviceIdentity::canonical();
    root["project_name"] = DeviceIdentity::projectName();
    root["fw_version"]   = DeviceIdentity::version();
    root["hw_revision"]  = DeviceIdentity::hwRevision();
    root["mac_address"]  = DeviceIdentity::macColon();
    root["hardware_uid"] = DeviceIdentity::uidHex32();
    root["flash_uid"]    = DeviceIdentity::flashUidHex();

    root["fs_total"] = ESPFS.totalBytes();
    root["fs_used"]  = ESPFS.usedBytes();
  }, 1792);
}

void SystemStatus::systemStatusForm(AsyncWebServerRequest* request) {
  // Snapshot heap BEFORE sendAdaptiveJson allocates the response doc.
  // The form's "Heap (live, at request entry)" row reads from these
  // captured values so it reports what the heap looked like when this
  // request arrived — not what it looks like a few millis later with
  // the response buffer already held. Eliminates ~2 KB of the ~3-5 KB
  // observer delta vs the mothership-checkin path; the rest is from
  // lwIP TCP_PCB / receive buffers held by the active HTTP session,
  // which can't be measured-around without lying.
  // sendAdaptiveJson starts at 1792 B and grows x2 if the rtos_tasks
  // table overflows the first attempt — no pool-full silent truncation.
  const uint32_t fh_entry = ESP.getFreeHeap();
  const uint32_t ma_entry = ESP.getMaxAllocHeap();
  sendAdaptiveJson(request, [this, fh_entry, ma_entry](JsonObject& o) {
    buildForm(o, fh_entry, ma_entry);
  }, 1792);
}

void SystemStatus::buildIdentityForm(JsonObject& root) {
  web::Form ide(root, "identity", "Device Identity");

  // Chip model + IDF/Arduino SDK rev. The HW-suffix postfix
  // ("ESP32-S3-N16R8V") joins runtime-detected sizes (flashMB +
  // psramMB) with compile-time-known buses (octal/quad). Same string
  // AutoUpdate sends as flv= in update-check URLs (case differs —
  // UI is upper, URL is lower).
  String chipSdk = DeviceIdentity::chipModelFull() + " / " + String(ESP.getSdkVersion());
  ide.add(web::Text("chip_sdk", chipSdk.c_str()).readOnly()
              .label("Chip / SDK").icon("Devices"));

  // Memory layout — runtime-detected sizes + compile-time-detected
  // SPI bus modes. Surfaces what fw the device can actually accept
  // an update for; useful when the same chip-model has multiple
  // PSRAM SKUs (S3 N16R8V vs S3 N8R2 — both "ESP32-S3" but binary-
  // incompatible at the SPIRAM init step).
  {
    String flashRow = String(DeviceIdentity::flashMB()) + " MB " +
                      (DeviceIdentity::flashIsOctal() ? "Octal-SPI" : "Quad-SPI");
    ide.add(web::Text("flash", flashRow.c_str()).readOnly()
                .label("Flash").icon("SdStorage"));

    int rmb = DeviceIdentity::psramMB();
    if (rmb > 0) {
      String psramRow = String(rmb) + " MB " +
                        (DeviceIdentity::psramIsOctal() ? "Octal-SPI" : "Quad-SPI");
      ide.add(web::Text("psram", psramRow.c_str()).readOnly()
                  .label("PSRAM").icon("Memory"));
    }
  }

  // Composite — same string the device puts in X.509 Subject CN and
  // sends as `deviceId` to the mothership. Operator-side admin UI
  // will display this exact string when correlating server records
  // with a physical device.
  ide.add(web::Text("device_id", DeviceIdentity::canonical().c_str()).readOnly()
              .label("Device ID").icon("Fingerprint"));

  ide.add(web::Text("project_name", DeviceIdentity::projectName().c_str()).readOnly()
              .label("Project").icon("Apps"));

  ide.add(web::Text("fw_version", DeviceIdentity::version().c_str()).readOnly()
              .label("Firmware version").icon("Update"));

  // Framework version — esp-rack library rev. Sourced from
  // WebManager's frameworkVersion() so it stays in sync with the
  // /api/v1/ui/manifest device.frameworkVersion. Skipped silently when
  // _web is null (defensive — SystemStatusModule always passes it).
  if (_web) {
    const char* fwVer = _web->frameworkVersion();
    if (fwVer && *fwVer) {
      String fw = String("esp-rack ") + fwVer;
      ide.add(web::Text("framework_version", fw.c_str()).readOnly()
                  .label("Framework").icon("Inventory"));
    }
  }

  // Only render HW Revision when the consumer actually declared one
  // via FACTORY_HW_REVISION — otherwise the row is just noise.
  if (DeviceIdentity::hwRevision().length() > 0) {
    ide.add(web::Text("hw_revision", DeviceIdentity::hwRevision().c_str()).readOnly()
                .label("HW Revision").icon("DeveloperBoard"));
  }

  ide.add(web::Text("mac_address", DeviceIdentity::macColon().c_str()).readOnly()
              .label("MAC (WiFi STA)").icon("DeviceHub"));

  // 32-hex eFuse hardware UID — immutable, fab-burned. Anti-clone
  // anchor; can't be changed in software. Empty/zeros on classic
  // ESP32 (no OPTIONAL_UNIQUE_ID block).
  ide.add(web::Text("hardware_uid", DeviceIdentity::uidHex32().c_str()).readOnly()
              .label("Hardware UID (eFuse)").icon("Tag"));

  // Flash chip 64-bit unique ID. Auxiliary fingerprint — changes if
  // someone swaps the flash chip (eFuse UID stays the same), so a
  // mismatch on the server side is a tamper signal. Zero when the
  // flash chip doesn't support the SFDP unique-ID command.
  ide.add(web::Text("flash_uid", DeviceIdentity::flashUidHex().c_str()).readOnly()
              .label("Flash UID").icon("SdStorage"));

  // Module composition table — per-module id+version from WebManager's
  // registry. Lives on Identity (not Resources) because it's a piece
  // of firmware-identity (which framework components shipped in this
  // build), not runtime utilisation. Skipped silently when _web is
  // null — defensive; SystemStatusModule always passes ctx.web post-
  // Builder, but null-tolerant ctor stays valid.
  if (_web) {
    ide.add(web::Table("modules")
                .col("module", "Module", "text")
                .col("version", "Version", "text")
                .icon("Extension"));
    JsonArray rows = ide.rowsOf("modules");
    _web->forEachModule([&](const char* id, const char* version) {
      JsonObject row = rows.createNestedObject();
      row["module"]  = id ? id : "";
      row["version"] = version ? version : "";
    });
  }
}

void SystemStatus::systemIdentityForm(AsyncWebServerRequest* request) {
  sendAdaptiveJson(request, [this](JsonObject& o){ buildIdentityForm(o); }, 1792);
}
