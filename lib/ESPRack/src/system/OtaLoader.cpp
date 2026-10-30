#include "system/OtaLoader.h"

#include <bootloader_random.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <string.h>

#include <atomic>

#include "App.h"
#include "DeviceIdentity.h"
#include "SecurityHash.h"
#include "SecurityManager.h"
#include "WebManager.h"
#include "power/PowerGuard.h"
#include "system/FirmwareUpdate.h"
#include "system/OtaWriter.h"

namespace ESPRack {
namespace OtaLoader {

namespace {

struct Layout {
  Scheme scheme = Scheme::None;
  const esp_partition_t* factory = nullptr;
  uint32_t slotSize = 0;
  char note[96] = "";
  char loaderVer[33] = "";
};

// The partition table doesn't change while the app runs: looked at once
// (thread-safe static init).
const Layout& layout() {
  static const Layout l = [] {
    Layout l;
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* factory =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    int slots = 0;
    const esp_partition_t* slot = nullptr;
    for (int i = ESP_PARTITION_SUBTYPE_APP_OTA_MIN; i < ESP_PARTITION_SUBTYPE_APP_OTA_MAX; ++i) {
      const esp_partition_t* p =
          esp_partition_find_first(ESP_PARTITION_TYPE_APP, static_cast<esp_partition_subtype_t>(i), nullptr);
      if (!p) continue;
      ++slots;
      if (!slot) slot = p;
    }
    if (factory && slots == 1 && running && running->address == slot->address) {
      esp_app_desc_t d;
      if (esp_ota_get_partition_description(factory, &d) == ESP_OK) {
        l.scheme = Scheme::Loader;
        l.factory = factory;
        l.slotSize = slot->size;
        strlcpy(l.loaderVer, d.version, sizeof(l.loaderVer));
      } else {
        strlcpy(l.note, "no loader in the factory partition: flash it by cable", sizeof(l.note));
      }
      return l;
    }
    // As OtaWriter sees it: a slot to write that is not the running one.
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (next && running && next->address != running->address) l.scheme = Scheme::Dual;
    if (factory && slots == 1 && running && running->address == factory->address)
      strlcpy(l.note, "the app runs in the factory partition, the loader's place", sizeof(l.note));
    return l;
  }();
  return l;
}

ReadyCheck s_ready;
std::atomic<bool> s_entering{false};  // static storage: RMW is fine
ota::Handoff s_handoff;               // the one being entered (up to the restart)
SecurityManager* s_sm = nullptr;      // set at install (setSecurityManager), read by the admin action
// The admin action's password check (Handoff adminKdf .. adminUser), entered at the client's disconnect.
constexpr size_t kAdminFrom = offsetof(ota::Handoff, adminKdf);
constexpr size_t kAdminTo = offsetof(ota::Handoff, apPass);
uint8_t s_pushAdmin[kAdminTo - kAdminFrom];

// The loader AP's password when this device's AP gives none: generated
// once per device, kept in NVS (begin(), before any other task reads it).
constexpr const char* kNvsApPassKey = "appass";
char s_apGen[ota::kApPassMax + 1] = "";

bool s_begun = false;
bool s_fromLoader = false;
bool s_haveResult = false;
ota::Result s_result;

void fillWifi(ota::Handoff& h) {
  wifi_mode_t mode = WIFI_MODE_NULL;
  wifi_config_t conf;
  if (esp_wifi_get_mode(&mode) == ESP_OK && (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) &&
      esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0]) {
    // Not terminated at full length.
    char ssid[sizeof(conf.sta.ssid) + 1] = {0};
    char pass[sizeof(conf.sta.password) + 1] = {0};
    memcpy(ssid, conf.sta.ssid, sizeof(conf.sta.ssid));
    memcpy(pass, conf.sta.password, sizeof(conf.sta.password));
    ota::setText(h.wifiSsid, ssid);
    ota::setText(h.wifiPass, pass);
    h.useShared = 0;
    return;
  }
  h.useShared = 1;  // no station config here: the loader's own copy
}

// The loader's AP password: this device's AP's while that AP runs with
// one, else the generated one ("" and false: none).
bool apPassword(char (&out)[ota::kApPassMax + 1], bool& deviceAp) {
  deviceAp = false;
  wifi_mode_t mode = WIFI_MODE_NULL;
  wifi_config_t conf;
  if (esp_wifi_get_mode(&mode) == ESP_OK && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) &&
      esp_wifi_get_config(WIFI_IF_AP, &conf) == ESP_OK && conf.ap.authmode != WIFI_AUTH_OPEN) {
    char pass[sizeof(conf.ap.password) + 1] = {0};  // not terminated at full length
    memcpy(pass, conf.ap.password, sizeof(conf.ap.password));
    if (ota::apPassOk(pass)) {
      ota::setText(out, pass);
      deviceAp = true;
      return true;
    }
  }
  ota::setText(out, s_apGen);
  return s_apGen[0] != 0;
}

// The AP password and the device id, for any handoff.
void fillDevice(ota::Handoff& h) {
  bool deviceAp = false;
  (void)apPassword(h.apPass, deviceAp);
  (void)ota::setText(h.deviceId, DeviceIdentity::canonical().c_str());
}

// The generated AP password: read, or made and stored on the first boot
// in the loader scheme (early boot: written at once, as BootGuard's). The
// RF isn't up yet, so the RNG alone would be pseudo-random (the classic
// ESP32 has no other entropy source): the bootloader's entropy source is
// on for the draw, as IDF allows in early boot (no ADC, I2S or Wi-Fi in
// use yet; with the Wi-Fi up the RNG needs none).
void loadApPassword() {
  nvs_handle_t nh;
  if (nvs_open(ota::kNvsNamespace, NVS_READWRITE, &nh) != ESP_OK) return;
  size_t len = sizeof(s_apGen);
  if (nvs_get_str(nh, kNvsApPassKey, s_apGen, &len) != ESP_OK || !ota::apPassOk(s_apGen)) {
    // 12 of 32 letters and digits (no l, o, 0, 1): 60 bits, easy to type.
    static const char kChars[] = "abcdefghijkmnpqrstuvwxyz23456789";
    uint8_t rnd[12];
    wifi_mode_t mode;
    const bool wifiUp = esp_wifi_get_mode(&mode) == ESP_OK;
    if (!wifiUp) bootloader_random_enable();
    esp_fill_random(rnd, sizeof(rnd));
    if (!wifiUp) bootloader_random_disable();
    for (size_t i = 0; i < sizeof(rnd); ++i) s_apGen[i] = kChars[rnd[i] & 31];
    s_apGen[sizeof(rnd)] = 0;
    if (nvs_set_str(nh, kNvsApPassKey, s_apGen) != ESP_OK || nvs_commit(nh) != ESP_OK) {
      log_w("[ota] the loader's AP password not stored: its AP stays open");
      s_apGen[0] = 0;
    }
  }
  nvs_close(nh);
}

bool writeHandoff() {
  nvs_handle_t nh;
  if (nvs_open(ota::kNvsNamespace, NVS_READWRITE, &nh) != ESP_OK) return false;
  const bool ok =
      nvs_set_blob(nh, ota::kNvsHandoffKey, &s_handoff, sizeof(s_handoff)) == ESP_OK && nvs_commit(nh) == ESP_OK;
  nvs_close(nh);
  return ok;
}

void eraseHandoff() {
  nvs_handle_t nh;
  if (nvs_open(ota::kNvsNamespace, NVS_READWRITE, &nh) != ESP_OK) return;
  if (nvs_erase_key(nh, ota::kNvsHandoffKey) == ESP_OK) (void)nvs_commit(nh);
  nvs_close(nh);
}

}  // namespace

Scheme scheme() { return layout().scheme; }

const char* schemeName(Scheme s) {
  switch (s) {
    case Scheme::None: return "none";
    case Scheme::Dual: return "dual";
    case Scheme::Loader: return "loader";
  }
  return "?";
}

const char* schemeNote() { return layout().note; }
const char* loaderVersion() { return layout().loaderVer; }
bool devKey() { return strstr(layout().loaderVer, "devkey") != nullptr; }
uint32_t appSlotSize() { return layout().slotSize; }

void setReadyCheck(ReadyCheck check) { s_ready = std::move(check); }

bool ready(String& why) {
  if (!s_ready) return true;
  why = "";
  if (s_ready(why)) return true;
  if (!why.length()) why = "the application says not now";
  return false;
}

void makePush(ota::Handoff& h, const char* adminPassword) {
  ota::clear(h, ota::Mode::Push);
  h.attemptsAllowed = ESPRACK_OTA_LOADER_ATTEMPTS;
  fillWifi(h);
  fillDevice(h);
  if (adminPassword && *adminPassword) {
    // As the security module stores a user's (SecurityHash.h): the loader
    // checks both kinds the same way.
    const String salt = SecurityHash::makeSalt();
    (void)setAdmin(h, "", salt.c_str(), SecurityHash::hashPassword(adminPassword, salt).c_str());
  }
}

bool setAdmin(ota::Handoff& h, const char* user, const char* saltHex, const char* hashHex) {
  const size_t chars = saltHex ? strlen(saltHex) : 0;
  uint8_t salt[ota::kAdminSaltMax];
  uint8_t key[ota::kAdminKeyLen];
  if (!chars || chars % 2 || chars / 2 > sizeof(salt) || !ota::fromHex(saltHex, salt, chars / 2) ||
      !ota::fromHex(hashHex, key, sizeof(key)))
    return false;
  h.adminKdf = static_cast<uint8_t>(ota::AdminKdf::Pbkdf2Sha256);
  h.adminSaltLen = static_cast<uint8_t>(chars / 2);
  h.adminIter = SecurityHash::ITERATIONS;
  memset(h.adminSalt, 0, sizeof(h.adminSalt));
  memcpy(h.adminSalt, salt, chars / 2);
  memcpy(h.adminKey, key, sizeof(key));
  (void)ota::setText(h.adminUser, user);  // shown only: cut is fine
  return true;
}

bool makePull(ota::Handoff& h, const char* url, uint32_t size, const char* sha256Hex, const char* sigHex,
              const char* token, const char* targetVersion, String& why) {
  ota::clear(h, ota::Mode::Pull);
  h.attemptsAllowed = ESPRACK_OTA_LOADER_ATTEMPTS;
  if (!url || !*url) {
    why = "no image url";
    return false;
  }
  if (!ota::setText(h.url, url)) {
    why = "image url longer than " + String(sizeof(h.url) - 1);
    return false;
  }
  if (!ota::setText(h.token, token)) {
    why = "download token too long";
    return false;
  }
  const uint32_t room = appSlotSize();
  if (size && room && size > room) {
    why = "image " + String(size) + " B, app slot " + String(room) + " B";
    return false;
  }
  h.size = size;
  if (sha256Hex && *sha256Hex && !ota::fromHex(sha256Hex, h.sha256, sizeof(h.sha256))) {
    why = "bad sha256";
    return false;
  }
  if (sigHex && *sigHex && !ota::sigFromHex(sigHex, h.sig, h.sigLen)) {
    why = "bad signature";
    return false;
  }
  // A production-key loader checks a pull's signature before its first
  // write from the handoff only (a trailer shows once the image is in) and
  // refuses one without: say so here, the app keeps running.
  if (scheme() == Scheme::Loader && !devKey() && (!h.sigLen || !ota::hasSha256(h))) {
    why = "the update loader has a production signing key: the server must give the image's SHA-256 and signature";
    return false;
  }
  (void)ota::setText(h.targetVersion, targetVersion);  // shown only: cut is fine
  fillWifi(h);
  fillDevice(h);
  return true;
}

bool canEnter(const ota::Handoff& h, String& why) {
  if (scheme() != Scheme::Loader) {
    why = String("no update loader on this board (") + schemeName(scheme()) + " scheme" +
          (schemeNote()[0] ? String(": ") + schemeNote() : String()) + ")";
    return false;
  }
  const auto mode = static_cast<ota::Mode>(h.mode);
  if (mode != ota::Mode::Push && mode != ota::Mode::Pull) {
    why = "no update mode in the handoff";
    return false;
  }
  if (mode == ota::Mode::Pull && !h.url[0]) {
    why = "no image url";
    return false;
  }
  if (!PowerGuard::writesAllowed()) {
    why = "flash writes paused (power)";
    return false;
  }
  if (FirmwareUpdate::active() || OtaWriter::busy()) {
    why = "another firmware update is under way";
    return false;
  }
  if (s_entering.load()) {
    why = "already entering update mode";
    return false;
  }
  return true;
}

bool enter(const ota::Handoff& h, const char* reason, String& why) {
  if (!canEnter(h, why)) {
    log_w("[ota] update mode refused: %s", why.c_str());
    return false;
  }
  bool expected = false;
  if (!s_entering.compare_exchange_strong(expected, true)) {
    why = "already entering update mode";
    return false;
  }
  s_handoff = h;
  ota::encode(s_handoff);
  log_w("[ota] restarting into the update loader (%s%s%s)", ota::modeName(s_handoff.mode),
        s_handoff.targetVersion[0] ? " -> " : "", s_handoff.targetVersion);
  // The handoff and the boot partition both in the restart's last step:
  // the otadata erase (and the loader image check before it) holds up no
  // module's window there (App.h afterShutdown).
  restartDevice(reason ? reason : kRestartUpdateMode, [] {
    if (!PowerGuard::writesAllowed()) {
      log_e("[ota] flash writes paused (power): update mode not entered");
      return;
    }
    if (!writeHandoff()) {
      log_e("[ota] NVS write failed: update mode not entered");
      return;
    }
    const esp_err_t e = esp_ota_set_boot_partition(layout().factory);
    if (e != ESP_OK) {
      log_e("[ota] loader not set to boot (%s): update mode not entered", esp_err_to_name(e));
      eraseHandoff();
    }
  });
}

void begin() {
  if (s_begun) return;  // early boot, one task (BootGuard::begin)
  s_begun = true;
  const Layout& l = layout();
  if (l.scheme == Scheme::Loader) {
    log_i("[ota] loader scheme: app slot %u B, loader %s%s", (unsigned)l.slotSize, l.loaderVer,
          devKey() ? " (dev signing key)" : "");
    loadApPassword();
  }
  if (l.note[0]) log_w("[ota] %s", l.note);
  // Only a board the loader ran on has the namespace: elsewhere this is
  // one failed open, nothing written.
  nvs_handle_t nh;
  if (nvs_open(ota::kNvsNamespace, NVS_READONLY, &nh) != ESP_OK) return;
  uint8_t buf[sizeof(ota::Result)];
  size_t len = sizeof(buf);
  const esp_err_t e = nvs_get_blob(nh, ota::kNvsResultKey, buf, &len);
  const bool result = e == ESP_OK || e == ESP_ERR_NVS_INVALID_LENGTH;  // a result of any size: the loader ran
  if (result) {
    s_fromLoader = true;
    const ota::Verdict v = e == ESP_OK ? ota::decode(buf, len, s_result) : ota::Verdict::BadSize;
    s_haveResult = v == ota::Verdict::Ok;
    if (!s_haveResult) log_w("[ota] the loader's result is unreadable (%s)", ota::verdictName(v));
  }
  // A handoff left behind (the loader erases it when done) holds a Wi-Fi
  // password and a token: not kept.
  len = 0;
  const bool handoff = nvs_get_blob(nh, ota::kNvsHandoffKey, nullptr, &len) == ESP_OK;
  nvs_close(nh);
  // The boot's writes go at once (as BootGuard's): no module holds write windows yet.
  if ((result || handoff) && nvs_open(ota::kNvsNamespace, NVS_READWRITE, &nh) == ESP_OK) {
    if (result) (void)nvs_erase_key(nh, ota::kNvsResultKey);
    if (handoff) (void)nvs_erase_key(nh, ota::kNvsHandoffKey);
    (void)nvs_commit(nh);
    nvs_close(nh);
  }
  if (s_haveResult) log_i("[ota] loader: %s", lastResultText().c_str());
}

bool fromLoader() { return s_fromLoader; }

bool lastResult(ota::Result& out) {
  if (!s_haveResult) return false;
  out = s_result;
  return true;
}

String lastResultText() {
  if (!s_haveResult) return String();
  char line[160];
  ota::describe(s_result, line, sizeof(line));
  return String(line);
}

bool loaderAp(String& ssid, String& pass, bool& deviceAp) {
  uint8_t mac[6] = {0};
  (void)esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);  // as the loader names its AP
  char name[24];
  snprintf(name, sizeof(name), "esprack-loader-%02X%02X", mac[4], mac[5]);
  ssid = name;
  char p[ota::kApPassMax + 1];
  const bool ok = apPassword(p, deviceAp);
  pass = p;
  return ok;
}

void setSecurityManager(SecurityManager* sm) { s_sm = sm; }

void registerManifest(WebManager* web) {
  if (!web || scheme() != Scheme::Loader) return;  // a dual board's manifest stays as it was
  WebActionSpec spec;
  spec.id = "system.enterUpdateMode";
  spec.title = "Enter update mode";
  spec.icon = "SystemUpdateAlt";
  spec.color = "warning";
  spec.method = "POST";
  spec.auth = WebAuthLevel::Admin;
  spec.confirm =
      "Restart into the update loader? The device is offline until a firmware is uploaded on its page "
      "(or the loader gives up and boots this firmware again).";
  spec.successMessage = "Restarting into update mode";
  spec.handler = [](AsyncWebServerRequest* r) {
    // async_tcp: checked and answered here, entered once the client has
    // gone (restartDevice parks its caller).
    const AsyncWebParameter* pw = r->getParam("password", true);
    ota::Handoff h;
    makePush(h, pw ? pw->value().c_str() : nullptr);
    if (!ota::hasAdmin(h) && s_sm) {
      // No password given: the requesting admin's own (their stored record).
      Authentication a = s_sm->authenticateRequest(r);
      if (a.authenticated && a.user)
        (void)setAdmin(h, a.user->username.c_str(), a.user->salt.c_str(), a.user->password.c_str());
    }
    String why;
    if (!canEnter(h, why)) {
      r->send(409, "application/json", "{\"ok\":false,\"error\":\"" + why + "\"}");
      return;
    }
    memcpy(s_pushAdmin, reinterpret_cast<const uint8_t*>(&h) + kAdminFrom, sizeof(s_pushAdmin));
    r->onDisconnect([] {
      ota::Handoff h;
      makePush(h, nullptr);
      memcpy(reinterpret_cast<uint8_t*>(&h) + kAdminFrom, s_pushAdmin, sizeof(s_pushAdmin));
      String why;
      (void)enter(h, kRestartUpdateMode, why);  // refused: logged
    });
    r->send(200, "application/json", "{\"ok\":true}");
  };
  web->registerAction(std::move(spec));
}

}  // namespace OtaLoader
}  // namespace ESPRack
