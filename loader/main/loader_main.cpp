// esprack-loader: the session (loader.h has the overview).

#include <stdio.h>
#include <string.h>

#include <esp_app_desc.h>
#include <esp_image_format.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "image_rules.h"
#include "image_writer.h"
#include "loader.h"
#include "ota_pubkey.h"

namespace loader {

Session g;

namespace {

const char* const TAG = "loader";
constexpr EventBits_t kEvAll = kEvDone | kEvBack | kEvUploadStart | kEvUploadEnd;
nvs_handle_t s_nvs = 0;
bool s_nvsUp = false;  // nvs_flash_init worked

void vformat(char* out, size_t cap, const char* fmt, va_list ap) {
  xSemaphoreTake(g.lock, portMAX_DELAY);
  vsnprintf(out, cap, fmt, ap);
  xSemaphoreGive(g.lock);
}

// ---- the records ----------------------------------------------------------

// The handoff and the Result of an earlier boot. NVS is never erased here:
// it holds the app's data (a failing init: no handoff, the upload page).
void loadRecords(ota::Result& prev) {
  ota::clear(prev);
  const esp_err_t e = nvs_flash_init();
  s_nvsUp = e == ESP_OK;
  if (e != ESP_OK || nvs_open(ota::kNvsNamespace, NVS_READWRITE, &s_nvs) != ESP_OK) {
    s_nvs = 0;
    ESP_LOGE(TAG, "NVS unavailable (0x%x): no handoff", unsigned(e));
    return;
  }
  static uint8_t raw[sizeof(ota::Handoff)];  // the larger record
  size_t len = sizeof(raw);
  if (nvs_get_blob(s_nvs, ota::kNvsHandoffKey, raw, &len) == ESP_OK) {
    const ota::Verdict v = ota::decode(raw, len, g.handoff);
    g.haveHandoff = v == ota::Verdict::Ok;
    if (!g.haveHandoff) ESP_LOGW(TAG, "handoff not usable: %s", ota::verdictName(v));
  }
  len = sizeof(raw);
  if (nvs_get_blob(s_nvs, ota::kNvsResultKey, raw, &len) != ESP_OK ||
      ota::decode(raw, len, prev) != ota::Verdict::Ok)
    ota::clear(prev);
}

// Whether ota_0 holds an image that boots, and its version.
void checkApp() {
  const esp_partition_pos_t pos = {g.slot->address, g.slot->size};
  esp_image_metadata_t md = {};
  g.appValid = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &md) == ESP_OK;
  esp_app_desc_t d = {};
  if (g.appValid && esp_ota_get_partition_description(g.slot, &d) == ESP_OK) ota::setText(g.appVersion, d.version);
}

void initResult(const ota::Result& prev) {
  ota::clear(g.result);
  if (!g.appValid && prev.outcome == uint8_t(ota::Outcome::FailedAfterWrite)) {
    // Back here in the middle of an update (a power cut, a crash after the
    // point of no return): go on counting.
    memcpy(g.result.from, prev.from, sizeof(prev.from));
    memcpy(g.result.to, prev.to, sizeof(prev.to));
    g.result.attempts = prev.attempts;
    g.priorMs = prev.durationMs;
  } else {
    ota::setText(g.result.from, g.appVersion);
  }
  if (g.haveHandoff && g.handoff.targetVersion[0]) ota::setText(g.result.to, g.handoff.targetVersion);
}

void storeResult(ota::Outcome o) {
  ota::Result& r = g.result;
  r.outcome = uint8_t(o);
  r.bytes = imageWriter().imageBytes();
  r.durationMs = sessionMs();
  char err[64], warn[96];
  copyTexts(err, sizeof(err), warn, sizeof(warn));
  // ok: a warning if there was one (e.g. an unsigned image went in)
  ota::setText(r.lastError, o == ota::Outcome::Ok ? warn : err);
  ota::encode(r);
  if (!s_nvs || nvs_set_blob(s_nvs, ota::kNvsResultKey, &r, sizeof(r)) != ESP_OK || nvs_commit(s_nvs) != ESP_OK)
    ESP_LOGE(TAG, "result not stored");
}

// Done with the handoff: it holds a Wi-Fi password and a token.
void dropHandoff() {
  if (!s_nvs || !g.haveHandoff) return;
  (void)nvs_erase_key(s_nvs, ota::kNvsHandoffKey);
  (void)nvs_commit(s_nvs);
}

[[noreturn]] void restartNow() {
  webStop();  // the last response goes out with the close
  esp_restart();
}

// ---- leaving the loader ---------------------------------------------------

// The old app again (nothing written, or nothing to write). Returns only
// if it doesn't boot.
void returnToApp(ota::Outcome o) {
  setPhase(Phase::Activating);
  const esp_err_t e = esp_ota_set_boot_partition(g.slot);  // validates it once more
  if (e != ESP_OK) {
    g.appValid = false;
    setError("the app does not boot (0x%x)", unsigned(e));
    return;
  }
  storeResult(o);
  dropHandoff();
  ESP_LOGW(TAG, "back to the app %s (%s)", g.appVersion, ota::outcomeName(uint8_t(o)));
  restartNow();
}

// The image just written. Returns only if it doesn't boot.
void activate() {
  setPhase(Phase::Activating);
  const esp_err_t e = esp_ota_set_boot_partition(g.slot);
  if (e != ESP_OK) {
    // Not set to boot (otadata): the session goes on, a pull or an upload
    // writes the next one.
    g.appValid = false;
    g.imageIn.store(false);
    setError("the new image does not boot (0x%x)", unsigned(e));
    return;
  }
  ota::setText(g.result.to, g.newVersion);
  storeResult(ota::Outcome::Ok);
  dropHandoff();
  ESP_LOGW(TAG, "%s installed: restarting into it", g.newVersion);
  restartNow();
}

// ---- waiting --------------------------------------------------------------

// Serves the session's events up to ticks (portMAX_DELAY: for good): an
// image in -> it boots; the page's "back" -> the app. An upload under way
// holds the clock; a failed one starts it over. Returns at the timeout, or
// as soon as one of the level bits `until` is set (no upload under way:
// meanwhile the wait leaves them out, a set level would wake it at once).
void serve(TickType_t ticks, EventBits_t until = 0) {
  bool uploading = false;
  TickType_t start = xTaskGetTickCount();
  for (;;) {
    if (!uploading && (xEventGroupGetBits(g.events) & until)) return;
    TickType_t wait = portMAX_DELAY;
    if (!uploading && ticks != portMAX_DELAY) {
      const TickType_t spent = xTaskGetTickCount() - start;
      if (spent >= ticks) return;
      wait = ticks - spent;
    }
    if (!uploading && phase() != Phase::Pausing) setPhase(Phase::Waiting);
    const EventBits_t b = xEventGroupWaitBits(g.events, kEvAll | (uploading ? 0 : until), pdFALSE, pdFALSE, wait);
    xEventGroupClearBits(g.events, b & kEvAll);  // events; the `until` levels stay
    if (b & kEvUploadStart) uploading = true;
    if (b & kEvUploadEnd) {
      uploading = false;
      start = xTaskGetTickCount();
    }
    if (b & kEvDone) {
      uploading = false;
      activate();
    }
    if (b & kEvBack) returnToApp(ota::Outcome::Aborted);
  }
}

bool joinNetwork(bool apIfDown) {
  setPhase(Phase::Joining);
  const ota::Handoff& h = g.handoff;
  const bool given = g.haveHandoff && !h.useShared && h.wifiSsid[0];
  const bool ok = net::join(given ? h.wifiSsid : "", given ? h.wifiPass : "", LOADER_JOIN_MS);
  if (!ok && apIfDown) net::startAp();
  return ok;
}

// No image to fetch (any more): the upload page, for good.
[[noreturn]] void waitForUpload() {
  if (!net::configured()) joinNetwork(true);
  else if (!net::up() && !net::apOn()) net::startAp();
  webStart();
  for (;;) serve(portMAX_DELAY);
}

// ---- the modes ------------------------------------------------------------

void runPush() {
  joinNetwork(true);
  webStart();
  ESP_LOGI(TAG, "waiting for an upload%s", g.appValid ? "" : " (no app to go back to)");
  for (;;) {
    serve(g.appValid ? pdMS_TO_TICKS(LOADER_IDLE_RETURN_MS) : portMAX_DELAY);
    if (g.appValid) {
      setError("no upload within %u min", unsigned(LOADER_IDLE_RETURN_MS / 60000));
      returnToApp(ota::Outcome::Aborted);
    }
  }
}

void runPull() {
  const ota::Handoff& h = g.handoff;
  // What the handoff alone tells, before the network.
  const char* why = nullptr;
  bool refuse = false;
  const bool sealed = h.sigLen && ota::hasSha256(h);  // hash and signature given
  if (strncmp(h.url, "http://", 7) != 0) {
    why = "not an http:// URL (this loader has no TLS)";
  } else if (h.size > g.slot->size) {
    why = "image larger than the app slot";
  } else if (g.prodKey && !sealed) {
    // A trailer's signature shows only once the image is written: with a
    // production key it is checked here, before the first write, or never.
    why = "production key: the pull needs SHA-256 and signature";
  } else if (sealed) {
    const char* v = signatureVerdict(h.sha256, h.sig, h.sigLen, &refuse);
    if (refuse) why = v;
    else if (*v) setWarning("%s", v);
  }
  if (why) {
    setError("%s", why);
    if (g.appValid) returnToApp(ota::Outcome::FailedBeforeWrite);
    storeResult(ota::Outcome::FailedAfterWrite);
    waitForUpload();
  }
  if (!joinNetwork(false)) {
    setError("Wi-Fi: no connection");
    if (g.appValid) returnToApp(ota::Outcome::FailedBeforeWrite);
    net::startAp();  // the upload page meanwhile
  }
  webStart();
  // Past the point of no return a pull is never given up (the same token,
  // §6 of the brief): a server or a network back after hours still gets
  // the device its app; an upload works all along.
  const uint16_t allowed = attemptsAllowed(h.attemptsAllowed);
  uint16_t failed = g.result.attempts;  // carried over from an earlier boot of this update
  for (;;) {
    if (!net::up()) {
      // No station: not an attempt. Uploads meanwhile (the AP), the next
      // try as soon as the station is back.
      setError("Wi-Fi: no connection");
      if (g.appValid) returnToApp(ota::Outcome::FailedBeforeWrite);
      if (!net::apOn()) net::startAp();
      setPhase(Phase::Pausing);
      serve(portMAX_DELAY, kEvNetUp);
      continue;
    }
    Pull r = Pull::Before;
    xSemaphoreTake(g.writer, portMAX_DELAY);  // an upload may hold it
    if (g.imageIn.load()) {
      xEventGroupClearBits(g.events, kEvDone | kEvUploadStart);  // no serve() saw them: none waits for its end
      r = Pull::Ok;  // that upload's image is in
    } else {
      ++g.result.attempts;
      r = pullOnce();
    }
    xSemaphoreGive(g.writer);
    if (r == Pull::Ok) {
      activate();  // returns only if it doesn't boot
      r = Pull::After;
    }
    // Nothing written and the old app still there: back to it.
    if (r == Pull::Before && g.appValid) returnToApp(ota::Outcome::FailedBeforeWrite);
    // Past the point of no return: keep trying.
    ++failed;
    storeResult(ota::Outcome::FailedAfterWrite);
    if (!net::up() && !net::apOn()) net::startAp();
    const uint32_t pause = retryPauseMs(failed);
    ESP_LOGW(TAG, "download %u failed: next in %u s%s", unsigned(failed), unsigned(pause / 1000),
             failed >= allowed ? " (stuck: an upload works too)" : "");
    setPhase(Phase::Pausing);
    serve(pdMS_TO_TICKS(pause));
  }
}

}  // namespace

// ---- Session helpers (loader.h) -------------------------------------------

const char* phaseName(Phase p) {
  static const char* const kNames[] = {"start",   "joining",   "waiting",    "pulling",
                                       "pausing", "uploading", "activating", "stopped"};
  const unsigned i = unsigned(p);
  return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
}

void setPhase(Phase p) { g.phase.store(uint8_t(p)); }
Phase phase() { return Phase(g.phase.load()); }

void setError(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vformat(g.error, sizeof(g.error), fmt, ap);
  va_end(ap);
  ESP_LOGE(TAG, "%s", g.error);
}

void setWarning(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vformat(g.warning, sizeof(g.warning), fmt, ap);
  va_end(ap);
  ESP_LOGW(TAG, "%s", g.warning);
}

void copyTexts(char* error, size_t errorCap, char* warning, size_t warningCap) {
  xSemaphoreTake(g.lock, portMAX_DELAY);
  snprintf(error, errorCap, "%s", g.error);
  snprintf(warning, warningCap, "%s", g.warning);
  xSemaphoreGive(g.lock);
}

uint32_t sessionMs() { return g.priorMs + uint32_t(esp_timer_get_time() / 1000); }

const uint8_t* pubKey() { return esprack_ota_pubkey; }
const char* keyId() { return ESPRACK_OTA_KEY_ID; }

}  // namespace loader

extern "C" void app_main(void) {
  using namespace loader;
  esp_log_level_set("loader", ESP_LOG_INFO);  // ours; IDF's own stay at the default (warnings)
  g.events = xEventGroupCreate();
  g.writer = xSemaphoreCreateMutex();
  g.lock = xSemaphoreCreateMutex();
  g.prodKey = !ESPRACK_OTA_KEY_DEV;
  ESP_LOGI("loader", "esprack-loader %s, signing key %s (%s)", esp_app_get_description()->version, keyId(),
           g.prodKey ? "production" : "dev");
  if (!g.prodKey) ESP_LOGW("loader", "dev signing key: unsigned or badly signed images go in with a warning");

  g.slot = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  if (!g.slot || g.slot == esp_ota_get_running_partition()) {
    // Never writes its own partition: a loader flashed into ota_0 stops here.
    ESP_LOGE("loader", "no app slot (ota_0) apart from this partition: nothing to do");
    setPhase(Phase::Stopped);
    return;
  }
  ota::Result prev;
  loadRecords(prev);
  checkApp();
  initResult(prev);
  const uint8_t mode = g.haveHandoff ? g.handoff.mode : uint8_t(ota::Mode::None);
  ESP_LOGI("loader", "mode %s, app %s", ota::modeName(mode), g.appValid ? g.appVersion : "(none)");
  // The loader itself crashed (a panic, a watchdog) while the old app is
  // still whole: give the device back to it rather than loop here.
  const esp_reset_reason_t why = esp_reset_reason();
  if (g.appValid && (why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT)) {
    setError("the loader restarted after a crash (reset reason %d)", int(why));
    returnToApp(ota::Outcome::FailedBeforeWrite);
  }
  if (!net::init(s_nvsUp)) {
    setError("Wi-Fi does not start");
    if (g.appValid) returnToApp(ota::Outcome::FailedBeforeWrite);
    setPhase(Phase::Stopped);
    return;
  }
  if (mode == uint8_t(ota::Mode::Pull)) runPull();
  else runPush();
}
