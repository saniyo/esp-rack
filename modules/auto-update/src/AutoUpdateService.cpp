// AutoUpdateService.cpp
#include "AutoUpdateService.h"

#include <WebManager.h>
#include <App.h>  // ESPRack::restartDevice (LIB-FIX-006)
#include <DeviceIdentity.h>
#include <system/FirmwareUpdate.h>
#include <system/LoopWait.h>

#ifdef ESP32
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <lwip/sockets.h>
#include <power/WriteGate.h>
#include <system/OtaLoader.h>
#include <system/OtaWriter.h>
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
  // A settings change (enabled, interval) is looked at on the next pass.
  addUpdateHandler([](const String&) { ESPRack::wakeLoop(); }, false);

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
  spec.auth = WebAuthLevel::Admin;
  // restRead / restUpdate omitted — registry mounts /api/v1/autoUpdate/form
  // and /api/v1/autoUpdate/state automatically.

  // REST buffer auto-sized (measured 0.6 KB live; fixed-count scalars,
  // status strings bounded <300 B — no runtime rows). Was hand-guessed
  // 4096, which also raised the heap-guard threshold 6x above need.
  _feature = web->registerFeature<AutoUpdateSettings>(
      std::move(spec), this,
      AutoUpdateSettings::buildForm, AutoUpdateSettings::update,
      WEBFEATURE_BUFFER_AUTO);

  WebTabSpec tab;
  tab.key = "autoUpdate";
  tab.title = "Auto Update";
  // Contributed to "system" — reference our typed feature by id; the
  // manifest emitter resolves the derived /api/v1/autoUpdate/form so a
  // feature-id rename can never silently desync this tab.
  tab.formOf = "autoUpdate";
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
#ifdef ESP32
  // The update loader's outcome (loader scheme), shown up to the next check.
  const String loader = ESPRack::OtaLoader::lastResultText();
  if (loader.length()) setLastResult(("loader " + loader).c_str());
#endif

  // Boot-time config dump removed — full state is visible on the
  // OTA tab in the UI; serial spam at every reboot just adds noise.
  // Functional logs (check fired, OTA progress, success/fail) stay
  // — they trigger only on real events.
}

// ====================================================
// LOOP — scheduler + timeout + state cleanup
// ====================================================
void AutoUpdateService::loop() {
  // --- The restart a timeout scheduled ---
  if (_restartPending) {
    if ((uint32_t)(millis() - _restartAtMs) < kTimeoutRestartMs) return;
    log_w("[AutoUpdate] OTA timeout — restarting");
    ESPRack::restartDevice(ESPRack::kRestartAutoUpdate);
  }

  // --- Timeout monitor for active OTA ---
  if (_otaState == AU_CHECKING || _otaState == AU_DOWNLOADING) {
    unsigned long elapsed = millis() - _otaStartMs;
    _state.otaElapsedSec = (int)(elapsed / 1000UL);

    uint32_t timeoutMs = (uint32_t)_state.updateTimeoutSec * 1000UL;
#ifdef ESP32
    // The task stops by itself at the deadline: it checks it while it
    // reads, once each slot is granted and before the image check, so
    // past it at most one write gate slot wait (maxWaitMs + maxWindowMs,
    // then the write goes; two with flash encryption) remains. Killing it is the last resort: a task deleted while
    // it waits in the write gate leaves its place there until the restart
    // below.
    const ESPRack::WriteGateConfig gate = ESPRack::WriteGate::config();
    const uint32_t tail = gate.maxWaitMs + gate.maxWindowMs + 5000;
    timeoutMs += tail > AUTO_UPDATE_KILL_GRACE_MS ? tail : (uint32_t)AUTO_UPDATE_KILL_GRACE_MS;
#endif
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
      // In kTimeoutRestartMs (the top of loop()), the loop free meanwhile.
      _restartAtMs = millis();
      _restartPending = true;
      return;
    }
  }

  // --- Reset terminal states so scheduler can fire again ---
  if (_otaTaskHandle == nullptr && (_otaState == AU_FAILED || _otaState == AU_TIMEOUT)) {
    setOtaState(AU_IDLE);
  }

  // --- Guards ---
  if (!_state.enabled || _state.checkInterval <= 0 || !_wifiConnected) return;
  if (_otaState != AU_IDLE) return;
  // Another image under way (the upload page, ArduinoOTA): the check waits
  // for it (they share the next OTA slot).
  if (ESPRack::FirmwareUpdate::active()) return;
#ifdef ESP32
  if (ESPRack::OtaWriter::busy()) return;
#endif

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

uint32_t AutoUpdateService::loopWaitMs(uint32_t nowMs) const {
  if (_restartPending) return ESPRack::LoopWait::dueIn(nowMs, _restartAtMs, kTimeoutRestartMs);
  // An update under way: its elapsed seconds and its deadline.
  if (_otaState == AU_CHECKING || _otaState == AU_DOWNLOADING) return 1000;
  if (_otaTaskHandle == nullptr && (_otaState == AU_FAILED || _otaState == AU_TIMEOUT)) return 0;
  if (!_state.enabled || _state.checkInterval <= 0 || !_wifiConnected || _otaState != AU_IDLE)
    return ESPRACK_LOOP_IDLE_WAIT_MS;
  // Another image under way: nothing tells us when it ends.
  if (ESPRack::FirmwareUpdate::active()) return ESPRACK_LOOP_MAX_WAIT_MS;
#ifdef ESP32
  if (ESPRack::OtaWriter::busy()) return ESPRACK_LOOP_MAX_WAIT_MS;
#endif
  return ESPRack::LoopWait::dueIn(nowMs, (uint32_t)_lastCheckMs, (uint32_t)_state.checkInterval * 60000UL);
}

void AutoUpdateService::otaTaskEntry(void* param) {
  AutoUpdateService* self = static_cast<AutoUpdateService*>(param);
  self->otaTaskRun();
  self->_otaTaskHandle = nullptr;
  ESPRack::wakeLoop();  // loop() resets a failed state, schedules the next check
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
  // One app slot: the update loader downloads it, after a restart.
  if (ESPRack::OtaLoader::scheme() == ESPRack::OtaLoader::Scheme::Loader) return handOffToLoader(url);
  // No reboot here (handleOtaResult restarts); FirmwareUpdate is reported
  // by the OtaWriter.
  return downloadImage(url);
#endif
}

#ifdef ESP32
namespace {

// The running image's SHA-256 in upper-case hex, as httpUpdate sent it.
String runningSha256() {
  uint8_t sha[32] = {0};
  if (esp_partition_get_sha256(esp_ota_get_running_partition(), sha) != ESP_OK) return String();
  char hex[2 * sizeof(sha) + 1];
  for (size_t i = 0; i < sizeof(sha); ++i) snprintf(hex + 2 * i, 3, "%02X", sha[i]);
  return String(hex);
}

}  // namespace

bool AutoUpdateService::otaPastDeadline() const {
  return millis() - _otaStartMs >= (uint32_t)_state.updateTimeoutSec * 1000UL;
}

bool AutoUpdateService::readStep(NetworkClient* tcp, uint8_t* buf, size_t n) {
  // NetworkClient::readBytes polls with delay(2) and restarts its timeout
  // at every byte; here the step has one timeout and the wait is select().
  const uint32_t t0 = millis();
  const uint32_t limitMs = (uint32_t)_state.updateTimeoutSec * 1000UL;
  size_t got = 0;
  while (got < n) {
    const int r = tcp->read(buf + got, n - got);  // what is there, no wait
    if (r < 0) return false;
    if (r > 0) {
      got += (size_t)r;
      continue;
    }
    const uint32_t now = millis();
    const uint32_t spent = now - t0;
    const uint32_t elapsed = now - (uint32_t)_otaStartMs;
    if (spent >= AUTO_UPDATE_HTTP_TIMEOUT_MS || elapsed >= limitMs) return false;
    uint32_t waitMs = AUTO_UPDATE_HTTP_TIMEOUT_MS - spent;
    if (limitMs - elapsed < waitMs) waitMs = limitMs - elapsed;
    const int fd = tcp->fd();
    if (fd < 0) return false;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    timeval tv;
    tv.tv_sec = waitMs / 1000;
    tv.tv_usec = (waitMs % 1000) * 1000;
    const int s = select(fd + 1, &rd, nullptr, nullptr, &tv);
    if (s < 0) return false;
    if (s > 0 && !tcp->available()) return false;  // readable, nothing to read: the peer closed
  }
  return true;
}

t_httpUpdate_return AutoUpdateService::otaFailed(int err, const String& msg) {
  _otaErr = err;
  _otaErrStr = msg;
  return HTTP_UPDATE_FAILED;
}

void AutoUpdateService::addImageRequestHeaders(HTTPClient& http) {
  // What Arduino's httpUpdate sent (servers may route on it).
  http.useHTTP10(true);  // no chunked transfer
  http.setTimeout(AUTO_UPDATE_HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setUserAgent("ESP32-http-Update");
  http.addHeader("Cache-Control", "no-cache");
  http.addHeader("x-ESP32-BASE-MAC", Network.macAddress());
  http.addHeader("x-ESP32-STA-MAC", WiFi.macAddress());
  http.addHeader("x-ESP32-AP-MAC", WiFi.softAPmacAddress());
  http.addHeader("x-ESP32-free-space", String(ESP.getFreeSketchSpace()));
  http.addHeader("x-ESP32-sketch-size", String(ESP.getSketchSize()));
  const String sketchMd5 = ESP.getSketchMD5();
  if (sketchMd5.length()) http.addHeader("x-ESP32-sketch-md5", sketchMd5);
  const String sketchSha = runningSha256();
  if (sketchSha.length()) http.addHeader("x-ESP32-sketch-sha256", sketchSha);
  http.addHeader("x-ESP32-chip-size", String(ESP.getFlashChipSize()));
  http.addHeader("x-ESP32-sdk-version", ESP.getSdkVersion());
  http.addHeader("x-ESP32-mode", "sketch");
  if (_state.currentVersion.length()) http.addHeader("x-ESP32-version", _state.currentVersion);
}

t_httpUpdate_return AutoUpdateService::checkImageAnswer(int code, int len) {
  if (code <= 0) return otaFailed(code, "HTTP error: " + HTTPClient::errorToString(code));
  switch (code) {
    case HTTP_CODE_OK:
      break;
    case HTTP_CODE_NO_CONTENT:
    case HTTP_CODE_NOT_MODIFIED:
      return HTTP_UPDATE_NO_UPDATES;
    case HTTP_CODE_NOT_FOUND:
      return otaFailed(HTTP_UE_SERVER_FILE_NOT_FOUND, "File Not Found (404)");
    case HTTP_CODE_FORBIDDEN:
      return otaFailed(HTTP_UE_SERVER_FORBIDDEN, "Forbidden (403)");
    default:
      return otaFailed(HTTP_UE_SERVER_WRONG_HTTP_CODE, "Wrong HTTP Code (" + String(code) + ")");
  }
  if (len <= 0) return otaFailed(HTTP_UE_SERVER_NOT_REPORT_SIZE, "Server Did Not Report Size");
  return HTTP_UPDATE_OK;
}

t_httpUpdate_return AutoUpdateService::handOffToLoader(const String& url) {
  _otaErr = 0;
  _otaErrStr = "";
  _otaTimedOut = false;
  // The image's answer only (its size; the server's SHA-256, signature,
  // token and version when it sends them): the loader downloads the body.
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) return otaFailed(HTTPC_ERROR_CONNECTION_REFUSED, "HTTP error: http.begin failed");
  addImageRequestHeaders(http);
  const char* keys[] = {"x-SHA256", "x-Signature", "x-Token", "x-Version"};
  http.collectHeaders(keys, 4);
  const int code = http.GET();
  const int len = http.getSize();
  const String sha = http.header("x-SHA256");
  const String sig = http.header("x-Signature");
  const String token = http.header("x-Token");
  const String version = http.header("x-Version");
  http.end();  // the body stays unread
  const t_httpUpdate_return answer = checkImageAnswer(code, len);
  if (answer != HTTP_UPDATE_OK) return answer;

  ESPRack::ota::Handoff h;  // 616 B of the update task's stack (enter() keeps its own copy)
  String why;
  // len: the file served, a plain image or a signed one (image + trailer):
  // the loader takes either as the size.
  if (!ESPRack::OtaLoader::makePull(h, url.c_str(), (uint32_t)len, sha.c_str(), sig.c_str(), token.c_str(),
                                    version.c_str(), why))
    return otaFailed(HTTP_UE_TOO_LESS_SPACE, "Update error: " + why);
  if (!ESPRack::OtaLoader::ready(why)) return otaFailed(ESP_ERR_INVALID_STATE, "not now: " + why);
  if (!sig.length()) log_w("[AutoUpdate] no signature from the server: the (dev-key) loader looks for one in the image");
  setOtaState(AU_SUCCESS);
  setLastResult("restarting into update mode");
  // Goes (the restart, no return), or why not.
  if (!ESPRack::OtaLoader::enter(h, ESPRack::kRestartAutoUpdate, why))
    return otaFailed(ESP_ERR_INVALID_STATE, "Update error: " + why);
  return HTTP_UPDATE_OK;
}

t_httpUpdate_return AutoUpdateService::downloadImage(const String& url) {
  _otaErr = 0;
  _otaErrStr = "";
  _otaTimedOut = false;
  // One image at a time (ota.begin() checks again before the first write).
  if (ESPRack::FirmwareUpdate::active() || ESPRack::OtaWriter::busy()) {
    return otaFailed(ESPRack::OtaWriter::kErrBusy, "Update error: another firmware update is under way");
  }

  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) return otaFailed(HTTPC_ERROR_CONNECTION_REFUSED, "HTTP error: http.begin failed");
  addImageRequestHeaders(http);
  const char* keys[] = {"x-MD5"};
  http.collectHeaders(keys, 1);

  const int code = http.GET();
  const int len = http.getSize();
  const t_httpUpdate_return answer = checkImageAnswer(code, len);
  if (answer != HTTP_UPDATE_OK) {
    http.end();
    return answer;
  }
  const uint32_t room = ESPRack::OtaWriter::freeSlotSize();
  if (!room) {
    http.end();
    return otaFailed(HTTP_UE_NO_PARTITION, "Partition Could Not be Found");
  }
  if ((uint32_t)len > room) {
    http.end();
    return otaFailed(HTTP_UE_TOO_LESS_SPACE, "Not Enough space");
  }
  const String md5 = http.header("x-MD5");

  // One 4 KB step at a time: read it, then write it in a slot of the
  // write gate (one sector erase). PSRAM when there is some.
  constexpr size_t kStep = ESPRack::OtaWriter::kStep;
  uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc_prefer(kStep, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!buf) {
    http.end();
    return otaFailed(ESP_ERR_NO_MEM, "Update error: no memory for a 4 KB buffer");
  }
  NetworkClient* tcp = http.getStreamPtr();
  ESPRack::OtaWriter& ota = _ota;  // kept up to the restart once it passed
  t_httpUpdate_return ret = HTTP_UPDATE_OK;
  const size_t total = (size_t)len;
  size_t done = 0;
  if (_progressCallback) _progressCallback(0, len);
  while (done < total) {
    if (otaPastDeadline()) {
      _otaTimedOut = true;
      ret = otaFailed(HTTPC_ERROR_READ_TIMEOUT, "timeout");
      break;
    }
    const size_t n = total - done < kStep ? total - done : kStep;
    if (!tcp || !readStep(tcp, buf, n)) {
      if (otaPastDeadline()) {
        _otaTimedOut = true;
        ret = otaFailed(HTTPC_ERROR_READ_TIMEOUT, "timeout");
      } else {
        ret = otaFailed(HTTPC_ERROR_READ_TIMEOUT, "Update error: stream read timeout");
      }
      break;
    }
    if (!done) {
      if (buf[0] != 0xE9) {  // an app image's magic byte
        ret = otaFailed(HTTP_UE_BIN_VERIFY_HEADER_FAILED, "Verify Bin Header Failed");
        break;
      }
      // Sequential mode erases as it goes: no flash write up to IDF 5.4;
      // from 5.5 (rollback) maybe an otadata sector: a slot of its own.
      const char* md5Hex = md5.length() ? md5.c_str() : nullptr;
      bool begun;
      if (ESPRack::OtaWriter::beginWrites()) {
        ESPRack::WriteSlot beginSlot;
        if (!beginSlot.ok()) {
          ret = otaFailed(ESP_ERR_INVALID_STATE, "Update error: flash writes paused (power)");
          break;
        }
        begun = ota.begin("auto-update", (uint32_t)total, md5Hex);
      } else {
        begun = ota.begin("auto-update", (uint32_t)total, md5Hex);
      }
      if (!begun) {
        ret = ota.error() == ESPRack::OtaWriter::kErrBadMd5
                  ? otaFailed(HTTP_UE_SERVER_FAULTY_MD5, "Wrong MD5")
                  : otaFailed(ota.error(), String("Update error: ") + ota.errorString());
        break;
      }
    }
    ESPRack::WriteSlot slot;
    if (otaPastDeadline()) {  // the wait for the slot took us past it
      _otaTimedOut = true;
      ret = otaFailed(HTTPC_ERROR_READ_TIMEOUT, "timeout");
      break;
    }
    if (!slot.ok()) {
      ret = otaFailed(ESP_ERR_INVALID_STATE, "Update error: flash writes paused (power)");
      break;
    }
    if (!ota.write(buf, n)) {
      ret = otaFailed(ota.error(), String("Update error: ") + ota.errorString());
      break;
    }
    slot.release();
    done += n;
    if (_progressCallback) _progressCallback((int)done, len);
  }
  if (ret == HTTP_UPDATE_OK && otaPastDeadline()) {
    _otaTimedOut = true;
    ret = otaFailed(HTTPC_ERROR_READ_TIMEOUT, "timeout");
  }
  if (ret == HTTP_UPDATE_OK) {
    // Check and close the image: a read of the whole image, a slot only
    // when it writes (flash encryption). Set to boot by the restart
    // (handleOtaResult: OtaWriter::restartInto, the buses stopped).
    bool ended = false;
    if (ESPRack::OtaWriter::endWrites()) {
      ESPRack::WriteSlot slot;
      if (!slot.ok()) {
        ret = otaFailed(ESP_ERR_INVALID_STATE, "Update error: flash writes paused (power)");
      } else {
        ended = ota.end();
      }
    } else {
      ended = ota.end();
    }
    if (ret == HTTP_UPDATE_OK && !ended) ret = otaFailed(ota.error(), String("Update error: ") + ota.errorString());
  }
  if (ret != HTTP_UPDATE_OK) ota.abort();  // an image that passed stays this writer's up to the restart
  heap_caps_free(buf);
  http.end();
  return ret;
}
#endif

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
      int err = _otaErr;
      String errStr = _otaErrStr;
#endif
      char lr[256];
      snprintf(lr, sizeof(lr), "%s failed (%d): %s", server, err, errStr.c_str());
      log_d("[AutoUpdate] %s", lr);
#ifdef ESP32
      // Stopped at the deadline: nothing left behind, no restart needed.
      setOtaState(_otaTimedOut ? AU_TIMEOUT : AU_FAILED);
#else
      setOtaState(AU_FAILED);
#endif
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
      // Exception (the update task, its last step): a second for "success"
      // to reach the page, as a wait on the task's own notification.
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
#ifdef ESP32
      _ota.restartInto(ESPRack::kRestartAutoUpdate);  // sets the image to boot, the buses stopped
#else
      ESPRack::restartDevice(ESPRack::kRestartAutoUpdate);
#endif
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
  ESPRack::wakeLoop();
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
