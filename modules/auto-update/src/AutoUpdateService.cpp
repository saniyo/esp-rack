// AutoUpdateService.cpp
#include "AutoUpdateService.h"

#include <WebManager.h>

#ifdef ESP32
#include <HTTPClient.h>
#elif defined(ESP8266)
#include <ESP8266HTTPClient.h>
#endif

static String buildUpdateUrl(const String& baseUrl, const String& basePlatform, const String& hwFlavor, const String& curVer) {
  String url = baseUrl;
  if (url.indexOf('?') == -1) url += "?";
  else url += "&";
  url += "dev=" + basePlatform;
  url += "&flv=" + hwFlavor;
  url += "&ver=" + curVer;
  return url;
}

static String computeHwSuffix() {
  String chip = "unknown";
#ifdef ESP32
  chip = ESP.getChipModel();
#elif defined(ESP8266)
  chip = "esp8266";
#endif
  chip.toLowerCase();
  chip.replace("-", "");
  chip.replace(" ", "");

  uint32_t flashBytes = ESP.getFlashChipSize();
  int flashMB = (int)((flashBytes + 512UL * 1024UL) / (1024UL * 1024UL));

  int psramMB = 0;
#ifdef ESP32
  uint32_t psramBytes = ESP.getPsramSize();
  psramMB = (int)((psramBytes + 512UL * 1024UL) / (1024UL * 1024UL));
  Serial.printf("[AutoUpdate] PSRAM: %u bytes (%d MB)\n", psramBytes, psramMB);
#endif

  String suffix = chip + "-n" + String(flashMB);
  if (psramMB > 0) {
    suffix += "r" + String(psramMB);
  }
  return suffix;
}

AutoUpdateService::AutoUpdateService(AsyncWebServer* server,
                                     ConfigManager* cfgMgr,
                                     SecurityManager* securityManager,
                                     const char* deviceName,
                                     const char* deviceVersion)
    : _httpEndpoint(AutoUpdateSettings::buildForm,
                    AutoUpdateSettings::update,
                    this, server, AUTO_UPDATE_SERVICE_PATH, securityManager),
      _cfg(cfgMgr,
           "autoUpdate",
           AUTO_UPDATE_SETTINGS_FILE,
           4096,
           this,
           AutoUpdateSettings::readPersistence,
           AutoUpdateSettings::updatePersistence,
           false /*autoSave*/),
      _wifiConnected(false),
      _checkedOnce(false),
      _lastCheckMs(0),
      _otaState(AU_IDLE),
      _otaStartMs(0),
      _otaTaskHandle(nullptr),
      _deviceName(deviceName),
      _deviceVersion(deviceVersion) {
  _state.enabled = true;
  _state.checkInterval = 60;
  _state.updateTimeoutSec = 120;
  _state.serverUrl = DEFAULT_UPDATE_SERVER_URL;
  _state.deviceName = _deviceName;
  _state.currentVersion = _deviceVersion;
  _state.otaStateStr = "idle";
  _state.lastResult = "";
  _state.lastResultTime = "";
  _state.otaElapsedSec = 0;

#ifdef ESP32
  WiFi.onEvent(
      std::bind(&AutoUpdateService::onStationModeGotIP, this, std::placeholders::_1, std::placeholders::_2),
      WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(
      std::bind(&AutoUpdateService::onStationModeDisconnected, this, std::placeholders::_1, std::placeholders::_2),
      WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent(
      std::bind(&AutoUpdateService::onStationModeLostIP, this, std::placeholders::_1, std::placeholders::_2),
      WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_LOST_IP);
#elif defined(ESP8266)
  _onStationModeGotIPHandler =
      WiFi.onStationModeGotIP(std::bind(&AutoUpdateService::onStationModeGotIP, this, std::placeholders::_1));
  _onStationModeDisconnectedHandler =
      WiFi.onStationModeDisconnected(
          std::bind(&AutoUpdateService::onStationModeDisconnected, this, std::placeholders::_1));
#endif
}

void AutoUpdateService::registerManifest(WebManager* web) {
  if (!web) return;
  WebTabSpec tab;
  tab.key = "autoUpdate";
  tab.title = "Auto Update";
  tab.restPath = AUTO_UPDATE_FORM_PATH;
  tab.postable = true;
  tab.auth = WebAuthLevel::Admin;
  web->addTabToFeature("system", tab);
}

void AutoUpdateService::begin() {
  (void)_cfg.ensureLoaded();
  _state.deviceName = _deviceName;
  _state.currentVersion = _deviceVersion;
  _state.hwSuffix = computeHwSuffix();

  String baseLower = String(_deviceName);
  baseLower.toLowerCase();
  _state.effectivePlatformId = baseLower + "-" + _state.hwSuffix;

  Serial.println(F("[AutoUpdate] ===== Config ====="));
  Serial.printf("  Device: %s  Ver: %s\n", _state.deviceName.c_str(), _state.currentVersion.c_str());
  Serial.printf("  HW: %s  PlatformID: %s\n", _state.hwSuffix.c_str(), _state.effectivePlatformId.c_str());
  Serial.printf("  Enabled: %s  Interval: %d min  Timeout: %d sec\n",
                _state.enabled ? "yes" : "no", _state.checkInterval, _state.updateTimeoutSec);
  Serial.printf("  Primary: %s\n  Fallback: %s\n", HARD_UPDATE_SERVER_URL, _state.serverUrl.c_str());
  Serial.println(F("[AutoUpdate] ================"));
}

// ====================================================
// LOOP — scheduler + timeout + state cleanup
// ====================================================
void AutoUpdateService::loop() {
  // --- Timeout monitor for active OTA ---
  if (_otaState == AU_CHECKING || _otaState == AU_DOWNLOADING) {
    unsigned long elapsed = millis() - _otaStartMs;
    _state.otaElapsedSec = (int)(elapsed / 1000UL);

    uint32_t timeoutMs = (uint32_t)_state.updateTimeoutSec * 1000UL;
    if (elapsed >= timeoutMs) {
      Serial.println(F("[AutoUpdate] TIMEOUT — killing OTA task, rebooting..."));
      setOtaState(AU_TIMEOUT);
      setLastResult("timeout — rebooting");

      if (_otaTaskHandle != nullptr) {
        vTaskDelete(_otaTaskHandle);
        _otaTaskHandle = nullptr;
      }
      // OTA timeout leaves WiFi/HTTP stack in bad state → reboot.
      // ESP32 OTA is partition-safe: partial writes never get booted.
      // vTaskDelay yields the scheduler instead of busy-blocking the loop.
      vTaskDelay(pdMS_TO_TICKS(1000));
      ESP.restart();
    }
  }

  // --- Reset terminal states so scheduler can fire again ---
  if (_otaTaskHandle == nullptr && (_otaState == AU_FAILED || _otaState == AU_TIMEOUT)) {
    setOtaState(AU_IDLE);
  }

  // --- Guards ---
  if (!_state.enabled || _state.checkInterval <= 0 || !_wifiConnected) return;
  if (_otaState != AU_IDLE) return;

  // --- Periodic scheduler ---
  unsigned long now = millis();
  unsigned long intervalMs = (unsigned long)_state.checkInterval * 60000UL;
  if (now - _lastCheckMs >= intervalMs) {
    Serial.printf("[AutoUpdate] Check fired (interval %d min)\n", _state.checkInterval);

    _otaStartMs = millis();
    setOtaState(AU_CHECKING);

    if (xTaskCreatePinnedToCore(
            otaTaskEntry, "ota_update", 6144, this, 1, &_otaTaskHandle, 1) != pdPASS) {
      Serial.printf("[AutoUpdate] ERROR: task create failed (free heap: %u)\n", ESP.getFreeHeap());
      setOtaState(AU_FAILED);
      setLastResult("task_create_failed");
      _lastCheckMs = millis();
    }
  }
}

void AutoUpdateService::otaTaskEntry(void* param) {
  AutoUpdateService* self = static_cast<AutoUpdateService*>(param);
  self->otaTaskRun();
  self->_otaTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

void AutoUpdateService::otaTaskRun() {
  checkForUpdate();
}

void AutoUpdateService::setOtaState(OtaState state) {
  _otaState = state;
  _state.otaStateStr = otaStateStr(state);
}

void AutoUpdateService::setLastResult(const char* result) {
  _state.lastResult = result;
  unsigned long uptimeSec = millis() / 1000UL;
  char buf[32];
  snprintf(buf, sizeof(buf), "%luh%lum%lus",
           uptimeSec / 3600, (uptimeSec % 3600) / 60, uptimeSec % 60);
  _state.lastResultTime = buf;
}

// ====================================================
// PREFLIGHT — GET /update with MAC header, read body
// ====================================================
int AutoUpdateService::preflightCheck(const String& url, uint32_t timeoutMs, String& responseBody) {
  if (WiFi.status() != WL_CONNECTED) {
    responseBody = "WiFi not connected";
    return -1;
  }

  WiFiClient client;
  HTTPClient http;

#if defined(ESP32)
  http.setConnectTimeout((int)min(timeoutMs, (uint32_t)10000));
#endif
  http.setTimeout((int)min(timeoutMs, (uint32_t)15000));

  if (!http.begin(client, url)) {
    responseBody = "http.begin failed";
    http.end();
    return -1;
  }

  // Add MAC header — server uses it for whitelist check
#ifdef ESP32
  http.addHeader("x-ESP32-STA-MAC", WiFi.macAddress());
#elif defined(ESP8266)
  http.addHeader("x-ESP8266-STA-MAC", WiFi.macAddress());
#endif

  int code = http.GET();

  if (code == 200) {
    // Update available — don't read binary body, just close
    responseBody = "Update available";
  } else if (code > 0) {
    // Non-200: read text body (403/304/400/500 messages)
    responseBody = http.getString();
    if (responseBody.length() > 200) {
      responseBody = responseBody.substring(0, 200);
    }
    responseBody.trim();
  } else {
    // Connection error
    responseBody = http.errorToString(code);
  }

  http.end();
  return code;
}

// ====================================================
// PERFORM UPDATE — actual OTA download
// ====================================================
t_httpUpdate_return AutoUpdateService::performUpdate(const String& url) {
  Serial.printf("[AutoUpdate] OTA download: %s\n", url.c_str());
  setOtaState(AU_DOWNLOADING);

#ifdef ESP8266
  ESPhttpUpdate.rebootOnUpdate(false);
  if (_progressCallback) {
    ESPhttpUpdate.onProgress([this](int cur, int total) { _progressCallback(cur, total); });
  }
  return ESPhttpUpdate.update(WiFi, url, _state.currentVersion);
#elif defined(ESP32)
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (_progressCallback) {
    httpUpdate.onProgress([this](int cur, int total) { _progressCallback(cur, total); });
  }
  WiFiClient client;
  client.setTimeout(_state.updateTimeoutSec);
  return httpUpdate.update(client, url, _state.currentVersion);
#endif
}

// ====================================================
// CHECK FOR UPDATE — preflight + OTA
// ====================================================
void AutoUpdateService::checkForUpdate() {
  _lastCheckMs = millis();

  if (!_state.enabled) {
    setOtaState(AU_IDLE);
    setLastResult("disabled");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setOtaState(AU_IDLE);
    setLastResult("no_wifi");
    return;
  }

  String basePlatform = String(_deviceName);
  basePlatform.toLowerCase();
  const String primaryUrl = buildUpdateUrl(String(HARD_UPDATE_SERVER_URL), basePlatform, _state.hwSuffix, _state.currentVersion);
  const String fallbackUrl = buildUpdateUrl(_state.serverUrl, basePlatform, _state.hwSuffix, _state.currentVersion);

  // --- 1) Primary server preflight ---
  String body;
  Serial.printf("[AutoUpdate] Preflight primary: %s\n", primaryUrl.c_str());
  int code = preflightCheck(primaryUrl, HARD_UPDATE_CHECK_BUDGET_MS, body);

  if (code == 200) {
    Serial.println(F("[AutoUpdate] Primary: update available, starting OTA..."));
    setLastResult("200: Downloading...");
    t_httpUpdate_return ret = performUpdate(primaryUrl);
    handleOtaResult(ret, "primary");
    return;
  }

  if (code > 0) {
    // Server responded but no update
    char lr[256];
    // 304 has no body per HTTP spec — use descriptive fallback
    if (code == 304) {
      snprintf(lr, sizeof(lr), "304: Up to date (device=%s)", _state.currentVersion.c_str());
    } else {
      snprintf(lr, sizeof(lr), "%d: %s", code, body.c_str());
    }
    Serial.printf("[AutoUpdate] Primary: %s\n", lr);

    if (code == 304 || code == 403 || code == 404) {
      // Terminal but NOT disabling — scheduler keeps running
      setOtaState(AU_IDLE);
      setLastResult(lr);
      if (code == 403) Serial.printf("[AutoUpdate] 403 — retry in %d min\n", _state.checkInterval);
      return;
    }
    // Other errors (400, 500) — try fallback
    setLastResult(lr);
  } else {
    Serial.printf("[AutoUpdate] Primary unreachable: %s\n", body.c_str());
  }

  // --- 2) Check timeout before fallback ---
  if ((millis() - _otaStartMs) >= (uint32_t)(_state.updateTimeoutSec * 1000)) {
    setOtaState(AU_TIMEOUT);
    setLastResult("timeout before fallback");
    return;
  }

  // --- 3) Fallback server preflight ---
  Serial.printf("[AutoUpdate] Preflight fallback: %s\n", fallbackUrl.c_str());
  code = preflightCheck(fallbackUrl, FALLBACK_UPDATE_CHECK_TIMEOUT_MS, body);

  if (code == 200) {
    Serial.println(F("[AutoUpdate] Fallback: update available, starting OTA..."));
    setLastResult("200: Downloading (fallback)...");
    t_httpUpdate_return ret = performUpdate(fallbackUrl);
    handleOtaResult(ret, "fallback");
    return;
  }

  if (code > 0) {
    char lr[256];
    if (code == 304) {
      snprintf(lr, sizeof(lr), "304: Up to date (device=%s)", _state.currentVersion.c_str());
    } else {
      snprintf(lr, sizeof(lr), "%d: %s", code, body.c_str());
    }
    Serial.printf("[AutoUpdate] Fallback: %s\n", lr);
    setOtaState(AU_IDLE);
    setLastResult(lr);
  } else {
    Serial.printf("[AutoUpdate] Fallback unreachable: %s\n", body.c_str());
    setOtaState(AU_IDLE);
    setLastResult("servers unreachable");
  }
}

// ====================================================
// Handle OTA result after performUpdate()
// ====================================================
void AutoUpdateService::handleOtaResult(t_httpUpdate_return ret, const char* server) {
  switch (ret) {
    case HTTP_UPDATE_FAILED: {
#ifdef ESP8266
      int err = ESPhttpUpdate.getLastError();
      String errStr = ESPhttpUpdate.getLastErrorString();
#elif defined(ESP32)
      int err = httpUpdate.getLastError();
      String errStr = httpUpdate.getLastErrorString();
#endif
      char lr[256];
      snprintf(lr, sizeof(lr), "%s failed (%d): %s", server, err, errStr.c_str());
      Serial.printf("[AutoUpdate] %s\n", lr);
      setOtaState(AU_FAILED);
      setLastResult(lr);
      break;
    }
    case HTTP_UPDATE_NO_UPDATES:
      Serial.printf("[AutoUpdate] %s: no update\n", server);
      setOtaState(AU_IDLE);
      setLastResult("304: No update");
      break;

    case HTTP_UPDATE_OK:
      Serial.printf("[AutoUpdate] %s: SUCCESS — rebooting\n", server);
      setOtaState(AU_SUCCESS);
      setLastResult("success");
      vTaskDelay(pdMS_TO_TICKS(1000));
      ESP.restart();
      break;
  }
}

// ====================================================
// WiFi event handlers
// ====================================================
#ifdef ESP32
void AutoUpdateService::onStationModeGotIP(WiFiEvent_t event, WiFiEventInfo_t info) {
  _wifiConnected = true;
  if (!_checkedOnce) {
    _checkedOnce = true;
    _lastCheckMs = 0;  // force immediate check
    Serial.println(F("[AutoUpdate] WiFi up — scheduling immediate check"));
  }
}

void AutoUpdateService::onStationModeDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  _wifiConnected = false;
}

void AutoUpdateService::onStationModeLostIP(WiFiEvent_t event, WiFiEventInfo_t info) {
  _wifiConnected = false;
}

#elif defined(ESP8266)
void AutoUpdateService::onStationModeGotIP(const WiFiEventStationModeGotIP& event) {
  _wifiConnected = true;
  if (!_checkedOnce) {
    _checkedOnce = true;
    _lastCheckMs = 0;
  }
}

void AutoUpdateService::onStationModeDisconnected(const WiFiEventStationModeDisconnected& event) {
  _wifiConnected = false;
}
#endif
