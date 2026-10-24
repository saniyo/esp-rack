#pragma once
#ifndef NTPSettingsService_h
#define NTPSettingsService_h

#include <StatefulService.h>
#include <ConfigManager.h>
#include <ConfigDelegate.h>
#include <FormBuilder.h>   // still used for updateValue() in update()
#include <web/Form.h>
#include <WebFeatureDelegate.h>
#include <time/TimeSync.h>

#include <time.h>
#ifdef ESP32
#include <WiFi.h>
#include <esp_idf_version.h>
// SNTP API split:
//  - reads (sntp_enabled / sntp_getservername) work on every IDF version
//    without TCPIP-core locking — they touch read-only globals.
//  - control writes (sntp_stop / sntp_init) MUST be wrapped in
//    LOCK_TCPIP_CORE() on IDF 5+ (Arduino-3.x, C6); the bare raw-API
//    asserts otherwise. esp_sntp_stop() is the locked wrapper added in
//    IDF 5; on IDF 4 (Arduino-2.x, S3) we still call sntp_stop() because
//    the wrapper is missing there.
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
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <sntp.h>
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
// SNTP poll interval. lwIP's default (3 h) equals the NTP freshness window of
// TimeSync, so authority would lapse right before each resync.
#ifndef NTP_SYNC_INTERVAL_MS
#define NTP_SYNC_INTERVAL_MS 3600000UL
#endif
#define NTP_SERVER_MAX 64  // incl. NUL
// Endpoint paths are derived by the registry now (C1): the typed feature
// mounts /api/v1/ntp/form (GET) and /api/v1/ntp/state (GET+POST) from
// spec.id — no per-module form-path #define. WS is unchanged (/ws/*).
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

  /* ----- clock validity (core TimeSync) ----- */
  // Two compact lines:
  //   clock      "Valid, ntp, ±1 s" (+ ", sources disagree")
  //   clock_diag "last step 12 ms, 0 rejected"
  struct ClockView {
    bool valid;
    char clock[64];
    char diag[48];
  };
  static ClockView clockView() {
    const ESPRack::TimeStatus st = ESPRack::TimeSync::instance().status();
    ClockView v;
    v.valid = st.valid;
    if (st.uncertaintyMs == UINT32_MAX) {
      snprintf(v.clock, sizeof(v.clock), "Not set");
    } else {
      snprintf(v.clock, sizeof(v.clock), "%s, %s, \u00b1%lu s%s", st.valid ? "Valid" : "Not valid",
               ESPRack::timeSourceName(st.source), (unsigned long)((st.uncertaintyMs + 500) / 1000),
               st.disputed ? ", sources disagree" : "");
    }
    snprintf(v.diag, sizeof(v.diag), "last step %lld ms, %u rejected", (long long)st.lastStepMs, (unsigned)st.rejects);
    return v;
  }

  /* ----- live WS snapshot (runtime + config echo) ----- */
  // `set_time` is deliberately absent: a tick must not overwrite the value
  // the user is picking.
  static void staRead(NTPSettings& s, JsonObject& root) {
    time_t now = time(nullptr);
    bool active = (bool)WIFI_SAFE_SNTP_ENABLED();
    const char* srv = WIFI_SAFE_SNTP_GETSERVER(0);

    root["state"]      = active ? "Active" : "Inactive";
    root["run_server"] = srv ? srv : "";
    root["local_time"] = formatTime(localtime(&now), "%b %d, %Y %H:%M:%S");
    root["utc_time"]   = formatTime(gmtime(&now),    "%b %d, %Y %H:%M:%S");
    root["uptime"]     = formatDurationSec((uint32_t)(millis() / 1000UL));

    ClockView cv = clockView();       // non-const: char[] is copied, const char* would be linked
    root["clock"]      = cv.clock;
    root["clock_diag"] = cv.diag;

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
    web::Form st(root, "status", "Status");
    // The `state` avatar switches to success (green) when NTP is actively
    // syncing, info (blue) otherwise — mirrors the legacy pre-refactor
    // highlight. Colour is baked into the REST form at fetch time; WS ticks
    // only refresh field VALUES, so the avatar colour transitions appear
    // on next page reload.
    st.add(web::Text("state", active ? "Active" : "Inactive").readOnly()
               .label("State").icon("Update").color(active ? "success" : "info"));
    st.add(web::Text("run_server", srv ? srv : "").readOnly()
               .label("Active server").icon("Dns"));
    st.add(web::Text("local_time", formatTime(localtime(&now), "%b %d, %Y %H:%M:%S").c_str())
               .readOnly().label("Local time").icon("AccessTime"));
    st.add(web::Text("utc_time", formatTime(gmtime(&now), "%b %d, %Y %H:%M:%S").c_str())
               .readOnly().label("UTC time").icon("SwapVerticalCircle"));
    st.add(web::Text("uptime", formatDurationSec((uint32_t)(millis() / 1000UL)).c_str())
               .readOnly().label("Uptime").icon("AvTimer"));
    const ClockView cv = clockView();
    st.add(web::Text("clock", cv.clock).readOnly().label("Clock").icon("Info")
               .color(cv.valid ? "success" : "warning"));
    st.add(web::Text("clock_diag", cv.diag).readOnly().label("Time sync").icon("Timeline"));
    // Operator time set (admin): pick a local time, the browser sends UTC
    // seconds; it outranks an external RTC and the soft-reset hold, never
    // GPS or NTP.
    st.add(web::DateTime("set_time", (int32_t)now).dateTime().label("Set clock to").icon("CalendarToday"));
    st.add(web::Action("a_set_time", "Set time").ref("system.setTime").withFields("epoch=set_time"));

    // SETTINGS
    web::Form set(root, "settings", "Settings");
    set.add(web::Switch("enabled", s.enabled).label("Enabled").icon("Sync"));
    set.add(web::Text("server", s.server.c_str()).label("NTP server").icon("Cloud"));
    set.add(web::TimeZones("tz_label", s.tzLabel.c_str()).label("Timezone").icon("Schedule"));
  }

  /* ----- Unified update ----- */
  static StateUpdateResult update(JsonObject& root, NTPSettings& s) {
    JsonObject src = root;
    if (root.containsKey("settings") && root["settings"].is<JsonObject>()) {
      src = root["settings"].as<JsonObject>();
    }

    // lwIP keeps the server name in a fixed buffer (NTP_SERVER_MAX)
    if (src.containsKey("server") && strlen(src["server"] | "") >= NTP_SERVER_MAX) {
      return StateUpdateResult::ERROR;
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
  char _serverBuf[2][NTP_SERVER_MAX]{};  // lwIP keeps a pointer to the SNTP server name (double-buffered)
  uint8_t _serverSel{0};

#ifdef ESP32
  void onStationModeGotIP(WiFiEvent_t event, WiFiEventInfo_t info);
  void onStationModeDisconnected(WiFiEvent_t event, WiFiEventInfo_t info);
#elif defined(ESP8266)
  WiFiEventHandler _onStationModeDisconnectedHandler;
  WiFiEventHandler _onStationModeGotIPHandler;
  void onStationModeGotIP(const WiFiEventStationModeGotIP& event);
  void onStationModeDisconnected(const WiFiEventStationModeDisconnected& event);
#endif

  void configureNTP();
#ifdef ESP32
  void configureNTPLocked(const NTPSettings& s);  // caller holds the service lock
#endif
};

#endif  // NTPSettingsService_h
