// AutoUpdateService.cpp
#include "AutoUpdateService.h"

#include <WebManager.h>
#include <DeviceIdentity.h>

#ifdef ESP32
#include <HTTPClient.h>
#elif defined(ESP8266)
#include <ESP8266HTTPClient.h>
#endif

static String buildUpdateUrl(const String& baseUrl,
                              const String& basePlatform,
                              const String& hwFlavor,
                              const String& curVer,
                              const String& deviceId,
                              const String& hwRev) {
  String url = baseUrl;
  if (url.indexOf('?') == -1) url += "?";
  else url += "&";
  url += "dev=" + basePlatform;
  url += "&flv=" + hwFlavor;
  url += "&ver=" + curVer;
  // Canonical device ID — "<project>-<mac>-<uid8>" — same string as
  // X.509 Subject CN and mothership deviceId. Server can use it for
  // per-device update channels (canary / staging / pinned firmware)
  // without parsing the cert.
  url += "&did=" + deviceId;
  // Hardware board revision (FACTORY_HW_REVISION). Lets the update
  // server return different firmware artefacts for rev-A vs rev-B
  // boards built off the same project. Omitted when empty.
  if (hwRev.length() > 0) {
    url += "&hw=" + hwRev;
  }
  return url;
}

// HW suffix used in the update URL's `flv=` and matched against the
// server's HW_FLAVOR_TAG_<flv> embedded in firmware binaries.
// Delegates to DeviceIdentity so the value is the SAME string that
// ends up in the cert, the mothership checkin payload, and the
// Identity tab readout — a divergence here would silently make
// AutoUpdate fetch the wrong firmware flavor (boot-loop on PSRAM
// mode mismatch). Format: "esp32s3-n16r8v" (chip model lowercased,
// dashes stripped, then "-" + DeviceIdentity::hwSuffix()).
static String computeHwSuffix() {
  String chip = DeviceIdentity::chipModel();
  chip.toLowerCase();
  chip.replace("-", "");
  chip.replace(" ", "");
  return chip + "-" + DeviceIdentity::hwSuffix();
}

AutoUpdateService::AutoUpdateService(ConfigManager* cfgMgr,
                                     const char* deviceName,
                                     const char* deviceVersion)
    : _cfg(cfgMgr,
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

  // Sub-tab of the compound `system` feature. WebManager owns the
  // endpoint binding + auth wrapper + proxy reach — same pipeline
  // as wifi / ntp / mqtt now.
  WebFeatureSpec spec;
  spec.id         = "autoUpdate";
  spec.title      = "Auto Update";
  spec.component  = "";  // sub-tab, system feature owns the menu slot
  spec.auth       = WebAuthLevel::Admin;
  spec.restRead   = AUTO_UPDATE_SERVICE_PATH;
  spec.restUpdate = AUTO_UPDATE_SERVICE_PATH;

  _feature = web->registerFeature<AutoUpdateSettings>(
      std::move(spec), this,
      AutoUpdateSettings::buildForm, AutoUpdateSettings::update,
      4096);

  WebTabSpec tab;
  tab.key = "autoUpdate";
  tab.title = "Auto Update";
  tab.restPath = AUTO_UPDATE_FORM_PATH;
  tab.postable = true;
  tab.auth = WebAuthLevel::Admin;
  tab.order = 80;
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

  // Boot-time config dump removed — full state is visible on the
  // OTA tab in the UI; serial spam at every reboot just adds noise.
  // Functional logs (check fired, OTA progress, success/fail) stay
  // — they trigger only on real events.
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
      log_d("[AutoUpdate] TIMEOUT — killing OTA task, rebooting...");
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
    log_d("[AutoUpdate] Check fired (interval %d min)", _state.checkInterval);

    _otaStartMs = millis();
    setOtaState(AU_CHECKING);

    if (xTaskCreatePinnedToCore(
            otaTaskEntry, "ota_update", 6144, this, 1, &_otaTaskHandle, 1) != pdPASS) {
      log_e("[AutoUpdate] ERROR: task create failed (free heap: %u)", ESP.getFreeHeap());
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

  // Add MAC header — server uses it for whitelist check.
  // DeviceIdentity::macColon() returns the same upper-case colon
  // format as WiFi.macAddress() so existing server-side whitelists
  // keep matching, but the value is now anchored to the same source
  // of truth as the cert / mothership / SettingValue placeholders.
  const String macHeader = DeviceIdentity::macColon();
#ifdef ESP32
  http.addHeader("x-ESP32-STA-MAC", macHeader);
#elif defined(ESP8266)
  http.addHeader("x-ESP8266-STA-MAC", macHeader);
#endif

  // Send canonical device ID alongside the MAC. Server can use this
  // for richer routing (project + per-device fingerprint) once the
  // backend learns about it; older servers ignore unknown headers.
  http.addHeader("x-ESPRack-Device-Id", DeviceIdentity::canonical());

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
  log_d("[AutoUpdate] OTA download: %s", url.c_str());
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
  const String deviceId = DeviceIdentity::canonical();
  const String hwRev    = DeviceIdentity::hwRevision();
  const String primaryUrl  = buildUpdateUrl(String(HARD_UPDATE_SERVER_URL), basePlatform, _state.hwSuffix, _state.currentVersion, deviceId, hwRev);
  const String fallbackUrl = buildUpdateUrl(_state.serverUrl,                basePlatform, _state.hwSuffix, _state.currentVersion, deviceId, hwRev);

  // --- 1) Primary server preflight ---
  String body;
  log_d("[AutoUpdate] Preflight primary: %s", primaryUrl.c_str());
  int code = preflightCheck(primaryUrl, HARD_UPDATE_CHECK_BUDGET_MS, body);

  if (code == 200) {
    log_d("[AutoUpdate] Primary: update available, starting OTA...");
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
    log_d("[AutoUpdate] Primary: %s", lr);

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
    log_e("[AutoUpdate] Primary unreachable: %s", body.c_str());
  }

  // --- 2) Check timeout before fallback ---
  if ((millis() - _otaStartMs) >= (uint32_t)(_state.updateTimeoutSec * 1000)) {
    setOtaState(AU_TIMEOUT);
    setLastResult("timeout before fallback");
    return;
  }

  // --- 3) Fallback server preflight ---
  log_d("[AutoUpdate] Preflight fallback: %s", fallbackUrl.c_str());
  code = preflightCheck(fallbackUrl, FALLBACK_UPDATE_CHECK_TIMEOUT_MS, body);

  if (code == 200) {
    log_d("[AutoUpdate] Fallback: update available, starting OTA...");
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
    log_d("[AutoUpdate] Fallback: %s", lr);
    setOtaState(AU_IDLE);
    setLastResult(lr);
  } else {
    log_e("[AutoUpdate] Fallback unreachable: %s", body.c_str());
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
      log_d("[AutoUpdate] %s", lr);
      setOtaState(AU_FAILED);
      setLastResult(lr);
      break;
    }
    case HTTP_UPDATE_NO_UPDATES:
      log_d("[AutoUpdate] %s: no update", server);
      setOtaState(AU_IDLE);
      setLastResult("304: No update");
      break;

    case HTTP_UPDATE_OK:
      log_d("[AutoUpdate] %s: SUCCESS — rebooting", server);
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
    log_d("[AutoUpdate] WiFi up — scheduling immediate check");
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
