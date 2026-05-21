#pragma once
#ifndef ESPRACK_DEVICE_IDENTITY_H
#define ESPRACK_DEVICE_IDENTITY_H

// DeviceIdentity — single source of truth for everything the device
// uses to identify itself: project name, MAC, hardware UID, and the
// composite canonical ID. All call sites that need any of these
// MUST come through here so format changes propagate atomically.
//
// The canonical ID layout is:
//
//   <project>-<mac12>-<uid8>
//
// where:
//   project  — esp_app_get_description()->project_name (set by CMake
//              project() / PIO build system). Falls back to "ESPRack"
//              when unavailable.
//   mac12    — wifi STA MAC, 12 hex chars lowercase, no separators.
//              Useful for DHCP / WiFi-debug correlation but is
//              software-overridable via esp_base_mac_addr_set — not
//              trustworthy as identity on its own.
//   uid8     — first 4 bytes (8 hex chars) of the eFuse-burned
//              OPTIONAL_UNIQUE_ID. Immutable, can't be changed in
//              software → anti-clone fingerprint. Falls back to
//              "00000000" on classic ESP32 (no OPTIONAL_UNIQUE_ID).
//
// This header is inline-only (no .cpp) so it can be included from
// any module without adding link dependencies.

#include <Arduino.h>

#if defined(ESP32)
#include <esp_mac.h>          // esp_read_mac
#include <esp_app_desc.h>     // esp_app_get_description
#include <esp_efuse.h>        // esp_efuse_read_field_blob
#include <esp_efuse_table.h>  // ESP_EFUSE_OPTIONAL_UNIQUE_ID
#include <esp_flash.h>        // esp_flash_read_unique_chip_id
#include <sdkconfig.h>        // CONFIG_SPIRAM_MODE_*, CONFIG_ESPTOOLPY_FLASHMODE_*
#endif

namespace DeviceIdentity {

// Stateful overrides for the device identity strings. All three are
// published by the App ctor right after Builder() is constructed —
// see App.cpp. esp_app_get_description() can't be the source on
// Arduino-ESP32 because the precompiled framework's project_name
// leaks through ("arduino-lib-builder") and the user's real app
// name is never embedded in the app desc. So Builder("MyApp",
// "v1.2.3") is THE authoritative source; HW revision comes from
// FACTORY_HW_REVISION compile-time macro (consumer-app's
// factory_settings.ini).
//
// Function-local statics keep the helpers inline-only — no .cpp
// file required to anchor the storage.
inline String& _projectOverride() {
  static String s;
  return s;
}
inline String& _versionOverride() {
  static String s;
  return s;
}
inline String& _hwRevisionOverride() {
  static String s;
  return s;
}

inline void setProjectName(const char* name) {
  _projectOverride() = (name && *name) ? String(name) : String();
}
inline void setVersion(const char* version) {
  _versionOverride() = (version && *version) ? String(version) : String();
}
inline void setHwRevision(const char* rev) {
  _hwRevisionOverride() = (rev && *rev) ? String(rev) : String();
}

inline String projectName() {
  // 1) Runtime override via setProjectName — populated by App ctor
  //    from Builder("...","...") first arg. Authoritative.
  if (_projectOverride().length() > 0) return _projectOverride();
  // 2) ESP-IDF app desc — works on native IDF builds, not on
  //    Arduino where the framework's lib-builder name leaks through.
#if defined(ESP32)
  const esp_app_desc_t* d = esp_app_get_description();
  if (d && d->project_name[0]
      && String(d->project_name) != "arduino-lib-builder") {
    return String(d->project_name);
  }
#endif
  // 3) Last-resort fallback so canonical() never returns an empty
  //    project segment.
  return String("ESPRack");
}

// App-level firmware version — the second arg to Builder("...","..."),
// usually wired from FACTORY_PROJECT_VERSION in factory_settings.ini.
// Consumed by mothership checkin's `fwVer` field. Distinct from
// ESPRACK_VERSION_STR (the esp-rack library release).
inline String version() {
  if (_versionOverride().length() > 0) return _versionOverride();
#if defined(ESP32)
  const esp_app_desc_t* d = esp_app_get_description();
  if (d && d->version[0]) return String(d->version);
#endif
  return String();
}

// Hardware board revision — operator-side identifier for the
// physical PCB rev (e.g. "rev-A"). Read from FACTORY_HW_REVISION
// macro in App ctor; empty when the consumer didn't set it.
// Surfaces in mothership checkin (`hwRev`), AutoUpdate URL query
// (`hw=...`), and the System / Identity tab.
inline String hwRevision() {
  return _hwRevisionOverride();
}

inline void macBytes(uint8_t out[6]) {
  for (int i = 0; i < 6; i++) out[i] = 0;
#if defined(ESP32)
  esp_read_mac(out, ESP_MAC_WIFI_STA);
#endif
}

// MAC as 12 hex chars, lowercase, no separators ("404cca4798f4").
// Used inside the canonical device ID — terse + URL-safe.
inline String macHex12() {
  uint8_t mac[6];
  macBytes(mac);
  char buf[16];
  snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// MAC as colon-separated hex, UPPER case ("40:4C:CA:47:98:F4") —
// matches WiFi.macAddress() output so this is a drop-in replacement
// for that call across the codebase. Used for human-facing display
// and HTTP headers (server whitelists / OTA update fingerprint).
inline String macColon() {
  uint8_t mac[6];
  macBytes(mac);
  char buf[20];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// Read 16-byte hardware unique ID from eFuse. Stays zero on classic
// ESP32 (no OPTIONAL_UNIQUE_ID block) — caller decides whether to
// use a fallback or accept zero bytes.
inline void uidBytes(uint8_t out[16]) {
  for (int i = 0; i < 16; i++) out[i] = 0;
#if defined(ESP32) && !CONFIG_IDF_TARGET_ESP32
  (void)esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID,
                                   out, 128);
#endif
}

// Full 128-bit unique ID as 32 hex chars. Useful for status display
// where the operator wants the entire fingerprint (audit logs).
inline String uidHex32() {
  uint8_t uid[16];
  uidBytes(uid);
  char buf[40];
  snprintf(buf, sizeof(buf),
           "%02x%02x%02x%02x%02x%02x%02x%02x"
           "%02x%02x%02x%02x%02x%02x%02x%02x",
           uid[0], uid[1], uid[2], uid[3],
           uid[4], uid[5], uid[6], uid[7],
           uid[8], uid[9], uid[10], uid[11],
           uid[12], uid[13], uid[14], uid[15]);
  return String(buf);
}

// First 4 bytes of UID as 8 hex chars — used inside canonical ID
// to keep the string compact (~30 chars total). Still gives ~4
// billion distinct values per project+MAC pair, which is plenty
// for anti-clone purposes.
inline String uidHex8() {
  uint8_t uid[16];
  uidBytes(uid);
  char buf[10];
  snprintf(buf, sizeof(buf), "%02x%02x%02x%02x",
           uid[0], uid[1], uid[2], uid[3]);
  return String(buf);
}

// Flash chip 64-bit unique ID (manufacturer-burned). Differs across
// physical chips even on otherwise-identical pre-built modules, so
// it complements the eFuse UID as a "did someone swap the flash?"
// fingerprint. Returns 0 if the chip doesn't support the SFDP unique
// ID command (rare on modern modules).
inline uint64_t flashUid64() {
  uint64_t uid = 0;
#if defined(ESP32)
  (void)esp_flash_read_unique_chip_id(esp_flash_default_chip, &uid);
#endif
  return uid;
}

// Flash UID as 16 hex chars. "0000000000000000" when unsupported.
inline String flashUidHex() {
  uint64_t uid = flashUid64();
  char buf[24];
  snprintf(buf, sizeof(buf),
           "%02x%02x%02x%02x%02x%02x%02x%02x",
           (unsigned)((uid >> 56) & 0xff),
           (unsigned)((uid >> 48) & 0xff),
           (unsigned)((uid >> 40) & 0xff),
           (unsigned)((uid >> 32) & 0xff),
           (unsigned)((uid >> 24) & 0xff),
           (unsigned)((uid >> 16) & 0xff),
           (unsigned)((uid >>  8) & 0xff),
           (unsigned)( uid        & 0xff));
  return String(buf);
}

// "<project>-<mac12>-<uid8>" — the device's canonical ID. Goes into
// X.509 Subject CN and as deviceId in mothership API payloads.
// Flash UID intentionally NOT in the canonical string — flash chips
// can be replaced, and the ID would change with a swap. eFuse UID is
// the immutable anchor; flash UID is auxiliary fingerprint shown on
// the System / Identity tab so an operator can spot a tamper.
inline String canonical() {
  return projectName() + "-" + macHex12() + "-" + uidHex8();
}


// ─────────────────────────────────────────────────────────────────
// Hardware-class identity — chip model + flash size + PSRAM size +
// PSRAM bus (quad/octal). Drives:
//   * hwSuffix()             — "n16r8v" / "n8r2" / "n4" — fleet-wide
//                              update channel discriminator
//   * chipModelFull()        — "ESP32-S3-N16R8V" — operator-facing
//                              display on Identity tab
// Both are derived once from runtime APIs (flash size, psram size)
// plus compile-time IDF config (octal-PSRAM mode is decided at build
// because Arduino-ESP32 requires the matching memory_type) — there's
// no runtime API that tells you "is this PSRAM wired octally" once
// the chip is up.

inline String chipModel() {
#if defined(ESP32)
  return String(ESP.getChipModel());
#else
  return String("unknown");
#endif
}

inline int flashMB() {
#if defined(ESP32)
  // Round to nearest MB so 16,777,216 B (16 MiB) reports as 16.
  uint32_t bytes = ESP.getFlashChipSize();
  return (int)((bytes + 512UL * 1024UL) / (1024UL * 1024UL));
#else
  return 0;
#endif
}

inline int psramMB() {
#if defined(ESP32)
  uint32_t bytes = ESP.getPsramSize();
  if (bytes == 0) return 0;
  return (int)((bytes + 512UL * 1024UL) / (1024UL * 1024UL));
#else
  return 0;
#endif
}

// True when PSRAM is wired on the Octal-SPI bus (S3 R8V / similar).
// False for Quad-SPI PSRAM (S3 R2 / R8 non-V) and when PSRAM is
// absent entirely. Decided by Arduino-ESP32 sdkconfig at build time
// via `board_build.arduino.memory_type = qio_opi` (octal) versus
// `qio_qspi` (quad) in platformio.ini. Runtime detection is not
// reliable because the PSRAM driver has already initialised in the
// configured mode by the time application code runs — if it's wrong
// the chip wouldn't boot at all.
inline bool psramIsOctal() {
#if defined(CONFIG_SPIRAM_MODE_OCT)
  return true;
#else
  return false;
#endif
}

// True when Flash is wired on the Octal-SPI bus (S3 "P4" boards
// with OPI flash, ESP32-P4 future). Vast majority of dev boards
// (including S3 N16R8V) use Quad-SPI flash even when PSRAM is octal.
inline bool flashIsOctal() {
#if defined(CONFIG_ESPTOOLPY_OCT_FLASH) || defined(CONFIG_ESPTOOLPY_FLASHMODE_OPI_STR)
  return true;
#else
  return false;
#endif
}

// "n16r8v" / "n8r2" / "n4" — Espressif-vendor convention used in
// module names like ESP32-S3-WROOM-1-N16R8V. Format:
//   n<flashMB>          — ALWAYS. Flash chip size.
//   r<psramMB>          — present iff PSRAM detected at runtime.
//   v                   — PSRAM is Octal-SPI (S3 "V" boards: N16R8V).
//                          Quad-SPI PSRAM is the silent default.
//
// CRITICAL: this string is the fleet-wide flavor key. AutoUpdate
// sends it as `flv=` to the update-server; the server picks the
// firmware whose embedded HW_FLAVOR_TAG_<flv> exactly matches.
// A binary built for an octal-PSRAM board will panic in the
// bootloader on a quad-PSRAM board (the SPIRAM driver fails to
// init and the heap allocator dies before app code runs), so a
// false match here = bricked update.
inline String hwSuffix() {
  String s = String("n") + String(flashMB());
  int rmb = psramMB();
  if (rmb > 0) {
    s += "r" + String(rmb);
    if (psramIsOctal()) s += "v";
  }
  return s;
}

// "ESP32-S3-N16R8V" — chip model with HW suffix appended in UPPER
// case for visual prominence on the Identity tab. The "-" separator
// echoes the dash inside the bare chip model so multi-word names
// like "ESP32-C6" stay legible.
inline String chipModelFull() {
  String base = chipModel();
  String suffix = hwSuffix();
  suffix.toUpperCase();
  return base + "-" + suffix;
}

}  // namespace DeviceIdentity

#endif  // ESPRACK_DEVICE_IDENTITY_H
