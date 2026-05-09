#pragma once
#ifndef NTPSettingsService_h
#define NTPSettingsService_h

#include <StatefulService.h>
#include <ConfigManager.h>
#include <ConfigDelegate.h>
#include <FormBuilder.h>
#include <WebFeatureDelegate.h>

#include <time.h>
#include <WiFi.h>
#include <esp_idf_version.h>
// SNTP API split between Arduino-2.x (IDF 4, S3) and Arduino-3.x
// (IDF 5+, C6). On IDF 5+ control writes MUST be wrapped in
// LOCK_TCPIP_CORE() — esp_sntp_stop() does that internally; the
// bare sntp_stop() asserts. Reads (sntp_enabled / sntp_getservername)
// work on every IDF version without locking.
#if ESP_IDF_VERSION_MAJOR >= 5
#include <esp_sntp.h>
#define WIFI_SAFE_SNTP_STOP()       esp_sntp_stop()
#define WIFI_SAFE_SNTP_ENABLED()    esp_sntp_enabled()
#define WIFI_SAFE_SNTP_GETSERVER(i) esp_sntp_getservername(i)
#else
#include <lwip/apps/sntp.h>
#define WIFI_SAFE_SNTP_STOP()       sntp_stop()
#define WIFI_SAFE_SNTP_ENABLED()    sntp_enabled()
#define WIFI_SAFE_SNTP_GETSERVER(i) sntp_getservername(i)
#endif

#ifndef FACTORY_NTP_ENABLED
#define FACTORY_NTP_ENABLED true
#endif
#ifndef FACTORY_NTP_TIME_ZONE_LABEL
#define FACTORY_NTP_TIME_ZONE_LABEL "Europe/London"
#endif
#ifndef FACTORY_NTP_TIME_ZONE_FORMAT
#define FACTORY_NTP_TIME_ZONE_FORMAT "GMT0BST,M3.5.0/1,M10.5.0"
#endif
#ifndef FACTORY_NTP_SERVER
#define FACTORY_NTP_SERVER "time.google.com"
#endif

#define NTP_FILE        "/config/ntpSettings.json"
#define NTP_FORM_PATH   "/rest/ntpForm"
#define NTP_WS_PATH     "/ws/ntpStatus"

class WebManager;

/* ================= SETTINGS ================= */
class NTPSettings {
 public:
  bool enabled = FACTORY_NTP_ENABLED;
  String tzLabel = FACTORY_NTP_TIME_ZONE_LABEL;
  String tzFormat = FACTORY_NTP_TIME_ZONE_FORMAT;
  String server = FACTORY_NTP_SERVER;

  static String formatTime(tm* t, const char* fmt) {
    char buf[25];
    strftime(buf, sizeof(buf), fmt, t);
    return String(buf);
  }

  static String formatDurationSec(uint32_t sec) {
    uint32_t d = sec / 86400; sec %= 86400;
    uint32_t h = sec / 3600;  sec %= 3600;
    uint32_t m = sec / 60;    sec %= 60;
    String out;
    if (d) { out += d; out += "d "; }
    if (h || d) { out += h; out += "h "; }
    if (m || h || d) { out += m; out += "m "; }
    out += sec; out += "s";
    return out;
  }

  /* ----- CONFIG persistence ----- */
  static void readConfig(NTPSettings& s, JsonObject& root) {
    root["enabled"]   = s.enabled;
    root["server"]    = s.server;
    root["tz_label"]  = s.tzLabel;
    root["tz_format"] = s.tzFormat;
  }

  /* ----- live WS snapshot (runtime + config echo) ----- */
  static void staRead(NTPSettings& s, JsonObject& root) {
    time_t now = time(nullptr);
    bool active = (bool)WIFI_SAFE_SNTP_ENABLED();
    const char* srv = WIFI_SAFE_SNTP_GETSERVER(0);

    root["state"]      = active ? "Active" : "Inactive";
    root["run_server"] = srv ? srv : "";
    root["local_time"] = formatTime(localtime(&now), "%b %d, %Y %H:%M:%S");
    root["utc_time"]   = formatTime(gmtime(&now),    "%b %d, %Y %H:%M:%S");
    root["uptime"]     = formatDurationSec((uint32_t)(millis() / 1000UL));

    root["enabled"]   = s.enabled;
    root["server"]    = s.server;
    root["tz_label"]  = s.tzLabel;
    root["tz_format"] = s.tzFormat;
  }

  static StateUpdateResult staUpd(JsonObject& in, NTPSettings& s) {
    return update(in, s);
  }

  /* ----- REST form (status + settings) ----- */
  static void buildForm(NTPSettings& s, JsonObject& root) {
    time_t now = time(nullptr);
    bool active = (bool)WIFI_SAFE_SNTP_ENABLED();
    const char* srv = WIFI_SAFE_SNTP_GETSERVER(0);

    // STATUS
    JsonArray st = FormBuilder::createForm(root, "status", "Status");
    // The `state` avatar switches to success (green) when NTP is actively
    // syncing, info (blue) otherwise — mirrors the legacy pre-refactor
    // highlight. Colour is baked into the REST form at fetch time; WS ticks
    // only refresh field VALUES, so the avatar colour transitions appear
    // on next page reload.
    FormBuilder::addTextField(st, "state",      AF::R, active ? "Active" : "Inactive",
                              label("State"),
                              icon("Update"), color(active ? "success" : "info"));
    FormBuilder::addTextField(st, "run_server", AF::R, srv ? srv : "",
                              label("Active server"),
                              icon("Dns"));
    FormBuilder::addTextField(st, "local_time", AF::R,
                              formatTime(localtime(&now), "%b %d, %Y %H:%M:%S").c_str(),
                              label("Local time"),
                              icon("AccessTime"));
    FormBuilder::addTextField(st, "utc_time",   AF::R,
                              formatTime(gmtime(&now),    "%b %d, %Y %H:%M:%S").c_str(),
                              label("UTC time"),
                              icon("SwapVerticalCircle"));
    FormBuilder::addTextField(st, "uptime",     AF::R,
                              formatDurationSec((uint32_t)(millis() / 1000UL)).c_str(),
                              label("Uptime"),
                              icon("AvTimer"));

    // SETTINGS
    JsonArray set = FormBuilder::createForm(root, "settings", "Settings");
    FormBuilder::addSwitchField(set, "enabled", AF::RW, s.enabled,
                                label("Enabled"), icon("Sync"));
    FormBuilder::addTextField  (set, "server",  AF::RW, s.server.c_str(),
                                label("NTP server"), icon("Cloud"));
    FormBuilder::addTimeZonesField(set, "tz_label", AF::RW, s.tzLabel.c_str(),
                                   label("Timezone"), icon("Schedule"));
  }

  /* ----- Unified update ----- */
  static StateUpdateResult update(JsonObject& root, NTPSettings& s) {
    JsonObject src = root;
    if (root.containsKey("settings") && root["settings"].is<JsonObject>()) {
      src = root["settings"].as<JsonObject>();
    }

    bool changed = false;
    changed |= FormBuilder::updateValue(src, "enabled", s.enabled);
    changed |= FormBuilder::updateValue(src, "server", s.server);
    changed |= FormBuilder::updateValue(src, "tz_label", s.tzLabel);
    changed |= FormBuilder::updateValue(src, "tz_format", s.tzFormat);

    return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
  }
};

/* ================= SERVICE ================= */
class NTPSettingsService : public StatefulService<NTPSettings> {
 public:
  NTPSettingsService(ConfigManager* cfgMgr);

  void registerManifest(WebManager* web);

  void begin();
  void loop();

 private:
  ConfigDelegate<NTPSettings> _cfg;
  WebFeatureEntry<NTPSettings>* _feature{nullptr};
  unsigned long _lastTickMs{0};

  void onStationModeGotIP(WiFiEvent_t event, WiFiEventInfo_t info);
  void onStationModeDisconnected(WiFiEvent_t event, WiFiEventInfo_t info);

  void configureNTP();
};

#endif  // NTPSettingsService_h
