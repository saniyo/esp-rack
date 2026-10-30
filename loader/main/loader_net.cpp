// esprack-loader: Wi-Fi. A station when one is known (the handoff's, kept
// in IDF's Wi-Fi storage for a later entry without one), else or besides an
// AP "esprack-loader-XXXX" (XXXX: the MAC's last two bytes): WPA2 with the
// handoff's apPass, open without one (no handoff: the boot button, or an
// app from before handoff version 2; the page warns).
// A lost station is joined again in the background: after 3 s, or 30 s
// while the AP is on (a station's scan takes the radio off the AP's
// channel).

#include <stdio.h>
#include <string.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include "loader.h"

namespace loader {
namespace net {

namespace {

const char* const TAG = "loader";
constexpr EventBits_t kUp = BIT0;

EventGroupHandle_t s_ev = nullptr;
esp_netif_t* s_sta = nullptr;
esp_timer_handle_t s_retry = nullptr;
bool s_started = false;
bool s_staOn = false;  // a station is configured
bool s_apOn = false;
bool s_apOpen = false;  // the AP has no password
bool s_nvs = false;  // IDF's Wi-Fi storage usable
char s_name[24] = {};  // "esprack-loader-XXXX": the AP's SSID and the host name

void retryLater() {
  if (!s_staOn) return;
  (void)esp_timer_stop(s_retry);
  (void)esp_timer_start_once(s_retry, (s_apOn ? 30000 : 3000) * 1000ULL);
}

void onEvent(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    if (s_staOn) (void)esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(s_ev, kUp);
    xEventGroupClearBits(g.events, kEvNetUp);
    retryLater();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto* e = static_cast<const ip_event_got_ip_t*>(data);
    ESP_LOGI(TAG, "station up: http://" IPSTR "/", IP2STR(&e->ip_info.ip));
    xEventGroupSetBits(s_ev, kUp);
    xEventGroupSetBits(g.events, kEvNetUp);
  }
}

// The radio's mode for what is on; started on the first call (the
// station's start event joins it).
void apply() {
  (void)esp_wifi_set_mode(s_staOn && s_apOn ? WIFI_MODE_APSTA : s_apOn ? WIFI_MODE_AP : WIFI_MODE_STA);
  if (!s_started) s_started = esp_wifi_start() == ESP_OK;
}

}  // namespace

bool init(bool nvs) {
  s_ev = xEventGroupCreate();
  if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK) return false;
  s_sta = esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  cfg.nvs_enable = nvs;  // the kept station
  s_nvs = nvs;
  esp_timer_create_args_t t = {};
  t.callback = [](void*) { (void)esp_wifi_connect(); };
  t.name = "sta-retry";
  if (esp_wifi_init(&cfg) != ESP_OK || esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||  // only join() keeps one
      esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr) != ESP_OK ||
      esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onEvent, nullptr) != ESP_OK ||
      esp_timer_create(&t, &s_retry) != ESP_OK)
    return false;
  uint8_t mac[6] = {};
  (void)esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(s_name, sizeof(s_name), "esprack-loader-%02X%02X", mac[4], mac[5]);
  (void)esp_netif_set_hostname(s_sta, s_name);
  return true;
}

bool join(const char* ssid, const char* pass, uint32_t timeoutMs) {
  const bool given = ssid && *ssid;
  // IDF takes and gives a station config only with the station in the mode.
  (void)esp_wifi_set_mode(s_apOn ? WIFI_MODE_APSTA : WIFI_MODE_STA);
  wifi_config_t c = {};
  if (given) {
    snprintf(reinterpret_cast<char*>(c.sta.ssid), sizeof(c.sta.ssid), "%s", ssid);
    snprintf(reinterpret_cast<char*>(c.sta.password), sizeof(c.sta.password), "%s", pass ? pass : "");
  } else if (esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK || !c.sta.ssid[0]) {
    ESP_LOGW(TAG, "no station known");
    if (s_apOn) apply();
    return false;
  }
  c.sta.threshold.authmode = WIFI_AUTH_OPEN;  // whatever the station offers
  c.sta.pmf_cfg.capable = true;
  c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  // The handoff's station is kept for a later entry without one (a button,
  // a crash series); IDF writes it only when it changed.
  if (given && s_nvs) (void)esp_wifi_set_storage(WIFI_STORAGE_FLASH);
  const esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &c);
  (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "station config: 0x%x", unsigned(e));
    return false;
  }
  ESP_LOGI(TAG, "joining %s", reinterpret_cast<const char*>(c.sta.ssid));
  s_staOn = true;
  const bool started = s_started;
  apply();
  if (started) (void)esp_wifi_connect();
  return waitUp(timeoutMs);
}

bool waitUp(uint32_t timeoutMs) {
  if (!s_staOn) return false;
  return xEventGroupWaitBits(s_ev, kUp, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeoutMs)) & kUp;
}

bool up() { return s_ev && (xEventGroupGetBits(s_ev) & kUp); }
bool configured() { return s_staOn; }

void startAp() {
  if (s_apOn) return;
  esp_netif_create_default_wifi_ap();
  wifi_config_t a = {};
  snprintf(reinterpret_cast<char*>(a.ap.ssid), sizeof(a.ap.ssid), "%s", s_name);
  a.ap.ssid_len = uint8_t(strlen(s_name));
  a.ap.channel = 1;
  a.ap.max_connection = 4;
  const char* pass = g.haveHandoff ? g.handoff.apPass : "";
  s_apOpen = !ota::apPassOk(pass);
  if (s_apOpen) {
    a.ap.authmode = WIFI_AUTH_OPEN;  // the page still asks for the admin password
  } else {
    a.ap.authmode = WIFI_AUTH_WPA2_PSK;
    snprintf(reinterpret_cast<char*>(a.ap.password), sizeof(a.ap.password), "%s", pass);
  }
  s_apOn = true;
  (void)esp_wifi_set_mode(s_staOn ? WIFI_MODE_APSTA : WIFI_MODE_AP);
  (void)esp_wifi_set_config(WIFI_IF_AP, &a);
  apply();
  ESP_LOGW(TAG, "AP %s (%s): http://192.168.4.1/", s_name, s_apOpen ? "open" : "WPA2");
}

bool apOn() { return s_apOn; }
bool apOpen() { return s_apOn && s_apOpen; }
const char* apName() { return s_name; }

}  // namespace net
}  // namespace loader
