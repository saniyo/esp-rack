#include <SystemStatus.h>

#include <FormBuilder.h>
#include <WebManager.h>

SystemStatus::SystemStatus(AsyncWebServer* server, SecurityManager* securityManager) {
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
}

void SystemStatus::registerManifest(WebManager* web) {
  if (!web) return;
  WebTabSpec tab;
  tab.key = "status";
  tab.title = "System Status";
  tab.restPath = SYSTEM_STATUS_FORM_PATH;
  tab.postable = false;
  tab.auth = WebAuthLevel::Authenticated;
  tab.order = 10;  // first tab — legacy parity
  web->addTabToFeature("system", tab);
}

void SystemStatus::buildForm(JsonObject& root) {
  JsonArray sta = FormBuilder::createForm(root, "status", "System Status");

  // Composite rows assembled at the backend — one row per concept with
  // already-formatted values. Legacy layout (SystemStatusForm.tsx mirror).
  // String() wrapping inside addTextField forces ArduinoJson to copy the
  // bytes, so local String lifetimes are safe.
  String device = String(ESP.getChipModel()) + " / " + String(ESP.getSdkVersion());
  FormBuilder::addTextField(sta, "device", AF::R, device.c_str(),
                            label("Chip / SDK"), icon("Devices"));

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

  root["fs_total"] = ESPFS.totalBytes();
  root["fs_used"] = ESPFS.usedBytes();

  response->setLength();
  request->send(response);
}

void SystemStatus::systemStatusForm(AsyncWebServerRequest* request) {
  AsyncJsonResponse* response = new AsyncJsonResponse(false, MAX_ESP_STATUS_SIZE * 4);
  JsonObject root = response->getRoot();
  buildForm(root);
  response->setLength();
  request->send(response);
}
