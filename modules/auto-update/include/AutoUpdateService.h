// AutoUpdateService.h
#ifndef AutoUpdateService_h
#define AutoUpdateService_h

#include <HttpEndpoint.h>
#include <ConfigManager.h>
#include <ConfigDelegate.h>
#include <Arduino.h>

#include <WiFi.h>
#include <HTTPUpdate.h>

#define AUTO_UPDATE_SETTINGS_FILE "/config/autoUpdateSettings.json"
#define AUTO_UPDATE_SERVICE_PATH "/rest/autoUpdateSettings"
#define AUTO_UPDATE_FORM_PATH    AUTO_UPDATE_SERVICE_PATH  // same endpoint serves the form schema

class WebManager;

// =========================================================
// Primary (hardcoded) update server (ALWAYS checked first)
// =========================================================
#ifndef HARD_UPDATE_SERVER_URL
#define HARD_UPDATE_SERVER_URL "http://5.58.3.220:5000/update"
#endif

#ifndef HARD_UPDATE_CHECK_BUDGET_MS
#define HARD_UPDATE_CHECK_BUDGET_MS 5000UL
#endif

#ifndef HARD_UPDATE_CHECK_ATTEMPTS
#define HARD_UPDATE_CHECK_ATTEMPTS 3
#endif

#ifndef FALLBACK_UPDATE_CHECK_TIMEOUT_MS
#define FALLBACK_UPDATE_CHECK_TIMEOUT_MS 5000UL
#endif

// =========================================================
// User-configurable fallback server default
// =========================================================
#ifndef DEFAULT_UPDATE_SERVER_URL
#define DEFAULT_UPDATE_SERVER_URL "http://192.168.10.202:5000/update"
#endif

#ifndef DEFAULT_DEVICE_NAME
#define DEFAULT_DEVICE_NAME "Molokijaclock"
#endif

#ifndef DEFAULT_DEVICE_VERSION
#define DEFAULT_DEVICE_VERSION "v1.0.3"
#endif

#include <FormBuilder.h>

// =========================================================
// OTA State Machine
// =========================================================
enum OtaState : uint8_t {
  AU_IDLE = 0,
  AU_CHECKING,
  AU_DOWNLOADING,
  AU_SUCCESS,
  AU_FAILED,
  AU_TIMEOUT
};

static const char* otaStateStr(OtaState s) {
  switch (s) {
    case AU_IDLE:        return "idle";
    case AU_CHECKING:    return "checking";
    case AU_DOWNLOADING: return "downloading";
    case AU_SUCCESS:     return "success";
    case AU_FAILED:      return "failed";
    case AU_TIMEOUT:     return "timeout";
    default:              return "unknown";
  }
}

class AutoUpdateSettings {
 public:
  bool enabled;
  int checkInterval;     // Period in minutes
  int updateTimeoutSec;  // OTA timeout in seconds (default 120)
  String serverUrl;      // user fallback server URL
  String deviceName;
  String currentVersion;
  String hwSuffix;            // e.g. "esp32s3-n8r2" (auto-detected, read-only)
  String effectivePlatformId; // deviceName + "-" + hwSuffix (read-only)

  // OTA status (read-only, not persisted)
  String otaStateStr;
  String lastResult;
  String lastResultTime;
  int otaElapsedSec;

  // =========================================================
  // FLAT JSON reader/writer — used by FSPersistence
  // Saves ONLY user-configurable fields as flat key:value pairs
  // =========================================================
  static void readPersistence(AutoUpdateSettings& settings, JsonObject& root) {
    root["enabled"] = settings.enabled;
    root["checkInterval"] = settings.checkInterval;
    root["updateTimeoutSec"] = settings.updateTimeoutSec;
    root["serverUrl"] = settings.serverUrl;
  }

  static StateUpdateResult updatePersistence(JsonObject& root, AutoUpdateSettings& settings) {
    bool changed = false;
    changed |= FormBuilder::updateValue(root, "enabled", settings.enabled);
    changed |= FormBuilder::updateValue(root, "checkInterval", settings.checkInterval);
    changed |= FormBuilder::updateValue(root, "updateTimeoutSec", settings.updateTimeoutSec);
    if (root.containsKey("serverUrl")) {
      String nv = root["serverUrl"].as<String>();
      if (nv != settings.serverUrl) { settings.serverUrl = nv; changed = true; }
    }
    return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
  }

  // =========================================================
  // FormBuilder reader — used by HttpEndpoint (UI GET)
  //
  // Form key "autoUpdate" matches the tab.key contributed by
  // AutoUpdateService::registerManifest to the compound 'system' feature.
  // Layout order: RW settings first (most-used controls), read-only device
  // identity next, last-OTA status last. Redundant fields collapsed into
  // composite rows assembled at the backend (see SystemStatus pattern).
  // =========================================================
  static void buildForm(AutoUpdateSettings& settings, JsonObject& root) {
    JsonArray fields = FormBuilder::createForm(root, "autoUpdate", "Configure automatic firmware updates");

    // ---- Main toggle (RW) ----
    FormBuilder::addSwitchField(fields, "enabled", AF::RW, settings.enabled,
                                icon("Sync"));

    // ---- Read-only device identity ----
    String device = settings.deviceName + " " + settings.currentVersion;  // e.g. "Molokijaclock v1.0.3"
    FormBuilder::addTextField(fields, "device", AF::R, device.c_str(), icon("Devices"));
    FormBuilder::addTextField(fields, "platform", AF::R, settings.hwSuffix.c_str(), icon("Memory"));
    FormBuilder::addTextField(fields, "primaryServer", AF::R, HARD_UPDATE_SERVER_URL, icon("Cloud"));

    // ---- Last-OTA status (R, composite) ----
    String otaStatus = settings.otaStateStr;
    if (settings.lastResult.length()) otaStatus += " — " + settings.lastResult;
    FormBuilder::addTextField(fields, "otaStatus", AF::R, otaStatus.c_str(), icon("Info"));

    String otaWhen;
    if (settings.lastResultTime.length()) {
      otaWhen = settings.lastResultTime;
      if (settings.otaElapsedSec > 0) otaWhen += " · " + String(settings.otaElapsedSec) + "s elapsed";
    } else {
      otaWhen = "never";
    }
    FormBuilder::addTextField(fields, "lastCheck", AF::R, otaWhen.c_str(), icon("Schedule"));

    // ---- Remaining RW config ----
    FormBuilder::addNumberField(fields, "checkInterval", AF::RW, settings.checkInterval,
                                minVal(1), maxVal(10080), format("0"),
                                unit("min"), icon("Schedule"));
    FormBuilder::addNumberField(fields, "updateTimeoutSec", AF::RW, settings.updateTimeoutSec,
                                minVal(30), maxVal(600), format("0"),
                                unit("s"), icon("Timer"));
    FormBuilder::addTextField  (fields, "serverUrl", AF::RW, settings.serverUrl.c_str(),
                                icon("Cloud"));
  }

  // =========================================================
  // UI update handler — used by HttpEndpoint (UI POST)
  // DynamicSettings sends flat changed fields: {"enabled": true, "checkInterval": 60}
  // Wrapped forms ({"autoUpdate": {...}} or legacy {"settings": {...}}) are
  // also accepted for backward compat.
  // =========================================================
  static StateUpdateResult update(JsonObject& root, AutoUpdateSettings& settings) {
    JsonObject src = root;
    if (root.containsKey("autoUpdate") && root["autoUpdate"].is<JsonObject>()) {
      src = root["autoUpdate"].as<JsonObject>();
    } else if (root.containsKey("settings") && root["settings"].is<JsonObject>()) {
      src = root["settings"].as<JsonObject>();
    }
    return updatePersistence(src, settings);
  }
};

class AutoUpdateService : public StatefulService<AutoUpdateSettings> {
 public:
  AutoUpdateService(AsyncWebServer* server,
                    ConfigManager* cfgMgr,
                    SecurityManager* securityManager,
                    const char* deviceName,
                    const char* deviceVersion);

  // Contribute the 'autoUpdate' tab to the compound 'system' feature.
  void registerManifest(WebManager* web);

  void begin();
  void loop();

  void checkForUpdate();

  using t_update_progress_callback = std::function<void(int progress, int total)>;
  void onProgress(t_update_progress_callback cb) { _progressCallback = cb; }

 private:
  // preflight check: GET with MAC header, returns HTTP code, fills responseBody
  int preflightCheck(const String& url, uint32_t timeoutMs, String& responseBody);

  // actual OTA update call
  t_httpUpdate_return performUpdate(const String& url);

  // handle OTA result (shared by primary/fallback)
  void handleOtaResult(t_httpUpdate_return ret, const char* server);

  // FreeRTOS OTA task
  static void otaTaskEntry(void* param);
  void otaTaskRun();
  void setOtaState(OtaState state);
  void setLastResult(const char* result);

 private:
  t_update_progress_callback _progressCallback;
  HttpEndpoint<AutoUpdateSettings> _httpEndpoint;
  ConfigDelegate<AutoUpdateSettings> _cfg;

  bool _wifiConnected;
  bool _checkedOnce;
  unsigned long _lastCheckMs;

  // OTA state machine
  volatile OtaState _otaState;
  unsigned long _otaStartMs;
  TaskHandle_t _otaTaskHandle;

  void onStationModeGotIP(WiFiEvent_t event, WiFiEventInfo_t info);
  void onStationModeDisconnected(WiFiEvent_t event, WiFiEventInfo_t info);
  void onStationModeLostIP(WiFiEvent_t event, WiFiEventInfo_t info);

  const char* _deviceName;
  const char* _deviceVersion;
};

#endif
