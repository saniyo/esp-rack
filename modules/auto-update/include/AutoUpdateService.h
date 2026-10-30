// AutoUpdateService.h
#ifndef AutoUpdateService_h
#define AutoUpdateService_h

#include <StatefulService.h>
#include <ConfigManager.h>
#include <ConfigDelegate.h>
#include <WebFeatureDelegate.h>
#include <Arduino.h>

#ifdef ESP32
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>  // t_httpUpdate_return, HTTP_UE_* (the image goes through ESPRack::OtaWriter)
#include <system/OtaWriter.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266httpUpdate.h>
#endif

#define AUTO_UPDATE_SETTINGS_FILE "/config/autoUpdateSettings.json"

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

// The image download: the longest wait for one 4 KB step's data (the whole
// step, not per byte), and how long past updateTimeoutSec the loop waits
// for the update task to stop by itself before it deletes it and restarts
// (at least the longest write gate slot wait plus a few seconds: the task
// checks the deadline while it reads, once each slot is granted and before
// the image check, so past it at most one slot wait remains).
#ifndef AUTO_UPDATE_HTTP_TIMEOUT_MS
#define AUTO_UPDATE_HTTP_TIMEOUT_MS 8000
#endif
#ifndef AUTO_UPDATE_KILL_GRACE_MS
#define AUTO_UPDATE_KILL_GRACE_MS 30000UL
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
  AutoUpdateService(ConfigManager* cfgMgr,
                    const char* deviceName,
                    const char* deviceVersion);

  // Contribute the 'autoUpdate' tab to the compound 'system' feature.
  void registerManifest(WebManager* web);

  void begin();
  void loop();
  // The next scheduled check, a running update's deadline (once a second),
  // the restart after a timeout; idle while disabled or offline (the Wi-Fi
  // events, the settings handler and the update task wake the loop).
  uint32_t loopWaitMs(uint32_t nowMs) const;

  void checkForUpdate();

  using t_update_progress_callback = std::function<void(int progress, int total)>;
  void onProgress(t_update_progress_callback cb) { _progressCallback = cb; }

 private:
  // preflight check: GET with MAC header, returns HTTP code, fills responseBody
  int preflightCheck(const String& url, uint32_t timeoutMs, String& responseBody);

  // actual OTA update call
  t_httpUpdate_return performUpdate(const String& url);
#ifdef ESP32
  // The download on the update task: HTTP GET (the headers httpUpdate
  // sent, redirects followed, 204/304 = no update, x-MD5 checked), then
  // the image through ESPRack::OtaWriter, each 4 KB step in a WriteSlot.
  t_httpUpdate_return downloadImage(const String& url);
  // The loader scheme (system/OtaLoader.h, one app slot): the image's
  // answer (size; x-SHA256 and x-Signature over the image bytes, x-Token,
  // x-Version when the server sends them, hex) becomes a pull handoff;
  // once the consumer's ready check agrees the device restarts into the
  // update loader, which downloads and checks it. A production-key loader
  // needs x-SHA256 and x-Signature; x-Token holds for its retries up to
  // the install. Returns only when it doesn't go.
  t_httpUpdate_return handOffToLoader(const String& url);
  void addImageRequestHeaders(HTTPClient& http);  // what httpUpdate sent
  t_httpUpdate_return checkImageAnswer(int code, int len);  // OK: a 200 with a size
  t_httpUpdate_return otaFailed(int err, const String& msg);
  bool otaPastDeadline() const;
  // n bytes of the image, waiting for the socket in select() (no polling);
  // false at the step's read timeout, the deadline, or the stream's end.
  bool readStep(NetworkClient* tcp, uint8_t* buf, size_t n);
  // The image under way or checked; lives up to the restart (restartInto).
  ESPRack::OtaWriter _ota;
  int _otaErr = 0;          // the last failure: an HTTPClient / HTTP_UE_* code, or an esp_err_t
  String _otaErrStr;
  bool _otaTimedOut = false;
#endif

  // handle OTA result (shared by primary/fallback)
  void handleOtaResult(t_httpUpdate_return ret, const char* server);

  // FreeRTOS OTA task
  static void otaTaskEntry(void* param);
  void otaTaskRun();
  void setOtaState(OtaState state);
  void setLastResult(const char* result);

 private:
  t_update_progress_callback _progressCallback;
  ConfigDelegate<AutoUpdateSettings> _cfg;
  WebFeatureEntry<AutoUpdateSettings>* _feature{nullptr};

  bool _wifiConnected;
  bool _checkedOnce;
  unsigned long _lastCheckMs;

  // OTA state machine
  volatile OtaState _otaState;
  unsigned long _otaStartMs;
  TaskHandle_t _otaTaskHandle;
  // After a timeout the restart waits a second (the page shows "timeout"
  // first): loop() and loopWaitMs() hold it, not a sleep on the loop task.
  static constexpr uint32_t kTimeoutRestartMs = 1000;
  bool _restartPending{false};
  uint32_t _restartAtMs{0};

#ifdef ESP32
  void onStationModeGotIP(WiFiEvent_t event, WiFiEventInfo_t info);
  void onStationModeDisconnected(WiFiEvent_t event, WiFiEventInfo_t info);
  void onStationModeLostIP(WiFiEvent_t event, WiFiEventInfo_t info);
#elif defined(ESP8266)
  WiFiEventHandler _onStationModeGotIPHandler;
  WiFiEventHandler _onStationModeDisconnectedHandler;

  void onStationModeGotIP(const WiFiEventStationModeGotIP& event);
  void onStationModeDisconnected(const WiFiEventStationModeDisconnected& event);
#endif

  const char* _deviceName;
  const char* _deviceVersion;
};

#endif
