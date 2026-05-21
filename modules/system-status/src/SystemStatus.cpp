#include <SystemStatus.h>

#include <FormBuilder.h>
#include <WebManager.h>
#include <DeviceIdentity.h>

SystemStatus::SystemStatus(AsyncWebServer* server, SecurityManager* securityManager, WebManager* web)
    : _web(web) {
  server->on(SYSTEM_STATUS_SERVICE_PATH,
             HTTP_GET,
             securityManager->wrapRequest(std::bind(&SystemStatus::systemStatus, this, std::placeholders::_1),
                                          AuthenticationPredicates::IS_AUTHENTICATED));

  // New form-schema endpoint consumed by the dynamic 'system' feature's
  // status tab. The response is {status:{description, fields:[…]}} with
  // read-only info rows + two embedded actionRef buttons at the bottom.
  server->on(SYSTEM_STATUS_FORM_PATH,
             HTTP_GET,
             securityManager->wrapRequest(std::bind(&SystemStatus::systemStatusForm, this, std::placeholders::_1),
                                          AuthenticationPredicates::IS_AUTHENTICATED));

  // Identity tab endpoint — pure DeviceIdentity readout. Same auth
  // level as Status (authenticated read) since it leaks fingerprint
  // material that lets an attacker correlate a captured cert to a
  // physical device.
  server->on(SYSTEM_IDENTITY_FORM_PATH,
             HTTP_GET,
             securityManager->wrapRequest(std::bind(&SystemStatus::systemIdentityForm, this, std::placeholders::_1),
                                          AuthenticationPredicates::IS_AUTHENTICATED));
}

void SystemStatus::registerManifest(WebManager* web) {
  if (!web) return;

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

void SystemStatus::buildForm(JsonObject& root) {
  JsonArray sta = FormBuilder::createForm(root, "status", "Resources");

  // Pure runtime telemetry: CPU / heap / PSRAM / sketch / flash / fs.
  // Identity-class fields (device id, MAC, chip model, SDK, framework
  // version, module list) live in the Identity tab — see
  // buildIdentityForm() / SYSTEM_IDENTITY_FORM_PATH. String() wrapping
  // inside addTextField forces ArduinoJson to copy the bytes, so local
  // String lifetimes are safe.

  String cpuFreq = String(ESP.getCpuFreqMHz()) + " MHz";
  FormBuilder::addTextField(sta, "cpu_frequency", AF::R, cpuFreq.c_str(),
                            label("CPU frequency"), icon("ShowChart"));

  String heap = String(ESP.getFreeHeap()) + " / " + String(ESP.getMaxAllocHeap()) + " bytes";
  FormBuilder::addTextField(sta, "heap_free_max_alloc", AF::R, heap.c_str(),
                            label("Heap (free / max alloc)"), icon("Memory"));

  if (ESP.getPsramSize() > 0) {
    String psram = String(ESP.getPsramSize()) + " / " + String(ESP.getFreePsram()) + " bytes";
    FormBuilder::addTextField(sta, "psram_size_free", AF::R, psram.c_str(),
                              label("PSRAM (size / free)"), icon("Apps"));
  }

  String sketch = String(ESP.getSketchSize()) + " / " + String(ESP.getFreeSketchSpace()) + " bytes";
  FormBuilder::addTextField(sta, "sketch_size_free", AF::R, sketch.c_str(),
                            label("Sketch (size / free)"), icon("DataUsage"));

  String flash = String(ESP.getFlashChipSize()) + " bytes / "
               + String((int)(ESP.getFlashChipSpeed() / 1000000)) + " MHz";
  FormBuilder::addTextField(sta, "flash_chip_size_speed", AF::R, flash.c_str(),
                            label("Flash (size / speed)"), icon("SdStorage"));

  size_t fsTotal = ESPFS.totalBytes();
  size_t fsUsed  = ESPFS.usedBytes();
  size_t fsFree  = fsTotal > fsUsed ? fsTotal - fsUsed : 0;
  String fs = String(fsUsed) + " / " + String(fsTotal) + " bytes (" + String(fsFree) + " free)";
  FormBuilder::addTextField(sta, "file_system_used_total", AF::R, fs.c_str(),
                            label("File system (used / total)"), icon("Folder"));

  // Module table moved to the Identity tab — what's installed is part
  // of "who this device is" (firmware composition fingerprint), not
  // resource utilisation. See buildIdentityForm().

  FormBuilder::addActionField(sta, "a_restart",      nullptr, AF::RW, actionRef("system.restart"));
  FormBuilder::addActionField(sta, "a_factoryReset", nullptr, AF::RW, actionRef("system.factoryReset"));
}

void SystemStatus::systemStatus(AsyncWebServerRequest* request) {
  AsyncJsonResponse* response = new AsyncJsonResponse(false, MAX_ESP_STATUS_SIZE);
  JsonObject root = response->getRoot();
  root["esp_platform"] = ESP.getChipModel();
  root["max_alloc_heap"] = ESP.getMaxAllocHeap();
  root["psram_size"] = ESP.getPsramSize();
  root["free_psram"] = ESP.getFreePsram();

  root["cpu_freq_mhz"] = ESP.getCpuFreqMHz();
  root["free_heap"] = ESP.getFreeHeap();
  root["sketch_size"] = ESP.getSketchSize();
  root["free_sketch_space"] = ESP.getFreeSketchSpace();
  root["sdk_version"] = ESP.getSdkVersion();
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
  // /rest/systemStatus can correlate this device with its server-
  // side record without parsing the cert.
  root["device_id"]    = DeviceIdentity::canonical();
  root["project_name"] = DeviceIdentity::projectName();
  root["fw_version"]   = DeviceIdentity::version();
  root["hw_revision"]  = DeviceIdentity::hwRevision();
  root["mac_address"]  = DeviceIdentity::macColon();
  root["hardware_uid"] = DeviceIdentity::uidHex32();
  root["flash_uid"]    = DeviceIdentity::flashUidHex();

  root["fs_total"] = ESPFS.totalBytes();
  root["fs_used"] = ESPFS.usedBytes();

  response->setLength();
  request->send(response);
}

void SystemStatus::systemStatusForm(AsyncWebServerRequest* request) {
  // Resources tab is now table-free (modules moved to Identity) — fits
  // comfortably in 2 KB: ~6 fields + 2 action refs.
  AsyncJsonResponse* response = new AsyncJsonResponse(false, MAX_ESP_STATUS_SIZE * 2);
  JsonObject root = response->getRoot();
  buildForm(root);
  response->setLength();
  request->send(response);
}

void SystemStatus::buildIdentityForm(JsonObject& root) {
  JsonArray ide = FormBuilder::createForm(root, "identity",
                                           "Device Identity");

  // Chip model + IDF/Arduino SDK rev. The HW-suffix postfix
  // ("ESP32-S3-N16R8V") joins runtime-detected sizes (flashMB +
  // psramMB) with compile-time-known buses (octal/quad). Same string
  // AutoUpdate sends as flv= in update-check URLs (case differs —
  // UI is upper, URL is lower).
  String chipSdk = DeviceIdentity::chipModelFull() + " / " + String(ESP.getSdkVersion());
  FormBuilder::addTextField(ide, "chip_sdk", AF::R, chipSdk.c_str(),
                            label("Chip / SDK"), icon("Devices"));

  // Memory layout — runtime-detected sizes + compile-time-detected
  // SPI bus modes. Surfaces what fw the device can actually accept
  // an update for; useful when the same chip-model has multiple
  // PSRAM SKUs (S3 N16R8V vs S3 N8R2 — both "ESP32-S3" but binary-
  // incompatible at the SPIRAM init step).
  {
    String flashRow = String(DeviceIdentity::flashMB()) + " MB " +
                      (DeviceIdentity::flashIsOctal() ? "Octal-SPI" : "Quad-SPI");
    FormBuilder::addTextField(ide, "flash", AF::R, flashRow.c_str(),
                              label("Flash"), icon("SdStorage"));

    int rmb = DeviceIdentity::psramMB();
    if (rmb > 0) {
      String psramRow = String(rmb) + " MB " +
                        (DeviceIdentity::psramIsOctal() ? "Octal-SPI" : "Quad-SPI");
      FormBuilder::addTextField(ide, "psram", AF::R, psramRow.c_str(),
                                label("PSRAM"), icon("Memory"));
    }
  }

  // Composite — same string the device puts in X.509 Subject CN and
  // sends as `deviceId` to the mothership. Operator-side admin UI
  // will display this exact string when correlating server records
  // with a physical device.
  FormBuilder::addTextField(ide, "device_id", AF::R,
                            DeviceIdentity::canonical().c_str(),
                            label("Device ID"), icon("Fingerprint"));

  FormBuilder::addTextField(ide, "project_name", AF::R,
                            DeviceIdentity::projectName().c_str(),
                            label("Project"), icon("Apps"));

  FormBuilder::addTextField(ide, "fw_version", AF::R,
                            DeviceIdentity::version().c_str(),
                            label("Firmware version"), icon("Update"));

  // Framework version — esp-rack library rev. Sourced from
  // WebManager's frameworkVersion() so it stays in sync with the
  // /rest/uiManifest device.frameworkVersion. Skipped silently when
  // _web is null (defensive — SystemStatusModule always passes it).
  if (_web) {
    const char* fwVer = _web->frameworkVersion();
    if (fwVer && *fwVer) {
      String fw = String("esp-rack ") + fwVer;
      FormBuilder::addTextField(ide, "framework_version", AF::R, fw.c_str(),
                                label("Framework"), icon("Inventory"));
    }
  }

  // Only render HW Revision when the consumer actually declared one
  // via FACTORY_HW_REVISION — otherwise the row is just noise.
  if (DeviceIdentity::hwRevision().length() > 0) {
    FormBuilder::addTextField(ide, "hw_revision", AF::R,
                              DeviceIdentity::hwRevision().c_str(),
                              label("HW Revision"), icon("DeveloperBoard"));
  }

  FormBuilder::addTextField(ide, "mac_address", AF::R,
                            DeviceIdentity::macColon().c_str(),
                            label("MAC (WiFi STA)"), icon("DeviceHub"));

  // 32-hex eFuse hardware UID — immutable, fab-burned. Anti-clone
  // anchor; can't be changed in software. Empty/zeros on classic
  // ESP32 (no OPTIONAL_UNIQUE_ID block).
  FormBuilder::addTextField(ide, "hardware_uid", AF::R,
                            DeviceIdentity::uidHex32().c_str(),
                            label("Hardware UID (eFuse)"), icon("Tag"));

  // Flash chip 64-bit unique ID. Auxiliary fingerprint — changes if
  // someone swaps the flash chip (eFuse UID stays the same), so a
  // mismatch on the server side is a tamper signal. Zero when the
  // flash chip doesn't support the SFDP unique-ID command.
  FormBuilder::addTextField(ide, "flash_uid", AF::R,
                            DeviceIdentity::flashUidHex().c_str(),
                            label("Flash UID"), icon("SdStorage"));

  // Module composition table — per-module id+version from WebManager's
  // registry. Lives on Identity (not Resources) because it's a piece
  // of firmware-identity (which framework components shipped in this
  // build), not runtime utilisation. Skipped silently when _web is
  // null — defensive; SystemStatusModule always passes ctx.web post-
  // Builder, but null-tolerant ctor stays valid.
  if (_web) {
    JsonObject tbl = FormBuilder::addTableField(ide, "modules", AF::R,
        col("module",  "Module",  "text"),
        col("version", "Version", "text"),
        icon("Extension"));
    JsonArray rows = tbl["modules"].as<JsonArray>();
    _web->forEachModule([&](const char* id, const char* version) {
      JsonObject row = rows.createNestedObject();
      row["module"]  = id ? id : "";
      row["version"] = version ? version : "";
    });
  }
}

void SystemStatus::systemIdentityForm(AsyncWebServerRequest* request) {
  AsyncJsonResponse* response = new AsyncJsonResponse(false, MAX_ESP_STATUS_SIZE * 2);
  JsonObject root = response->getRoot();
  buildIdentityForm(root);
  response->setLength();
  request->send(response);
}
