#pragma once
#ifndef ESPRACK_SYSTEM_OTA_LOADER_H
#define ESPRACK_SYSTEM_OTA_LOADER_H

// OtaLoader: firmware updates through the update loader, for boards whose
// partition table has one app slot (docs/plans/lib-ota-loader.md).
//
// The partition table picks the scheme, the same code runs on every chip
// (ESP32, C3, C6, S3) and flash size:
//   Dual    two OTA slots (or a factory app with a free slot): the app
//           writes the other slot itself (OtaWriter), as always
//   Loader  a factory partition holding the loader + exactly one OTA slot,
//           the app running from that slot: the app hands an update to the
//           loader and restarts into it (downtime: the whole transfer)
//   None    nothing to write into
// A dual board behaves exactly as before; nothing switches by itself.
//
// Entering the loader (only in the Loader scheme):
//   - the admin action system.enterUpdateMode (the upload page's "Restart
//     into update mode", BootGuard's safe mode): a push handoff, the loader
//     serves its upload page;
//   - auto-update / mothership: a pull handoff (url, size, SHA-256,
//     signature, token) once the consumer's ready check says now is safe;
//   - BootGuard: after limit + 2 crash boots in a row (system/BootGuard.h).
// enter() writes the handoff (system/OtaHandoff.h) and sets the factory
// partition to boot in the restart's afterShutdown step (App.h: after
// every module's onShutdown, in a write gate slot), then the chip resets.
//
//   OtaLoader::setReadyCheck([](String& why) {          // consumer, at setup
//     if (inverterCurrentA() > 0.5f) { why = "inverter busy"; return false; }
//     return true;
//   });
//
// The loader's result (system/OtaHandoff.h Result) is read once at boot
// (begin(), from BootGuard::begin) and erased; lastResult() hands it out
// for the rest of the boot. A boot that came from the loader is no crash
// (BootGuard). ESP32 family only.
//
// What a handoff carries besides the update (OtaHandoff.h, version 2):
//   - the upload page's password check: the stored PBKDF2 record of the
//     admin who entered update mode (SecurityHash.h: salt, rounds, hash;
//     the loader derives the same), or one made from a password given
//     with the action; none (no security module, a machine token, a pull,
//     the crash loop): the loader's page is open and says so;
//   - the loader's fallback AP password (loaderAp()): this device's own
//     AP password when its AP runs with one, else one generated once per
//     device (NVS, shown on the upload page); the crash loop's handoff
//     (early boot, no AP up yet) gives none: the loader's AP is open;
//   - the device id the loader sends the image server.

#include <Arduino.h>

#include <functional>

#include "system/OtaHandoff.h"

class SecurityManager;
class WebManager;

#ifndef ESPRACK_OTA_LOADER_ATTEMPTS
#define ESPRACK_OTA_LOADER_ATTEMPTS 5  // downloads after the first write before the loader calls the update stuck (it goes on)
#endif

namespace ESPRack {

// Restart reasons (<= 23 chars survive the reboot).
static constexpr const char* kRestartUpdateMode = "update-mode";  // into the loader
static constexpr const char* kRestartCrashLoop = "crash-loop";    // BootGuard into the loader

namespace OtaLoader {

enum class Scheme : uint8_t { None, Dual, Loader };

// From the partition table and the running partition, found once.
Scheme scheme();
const char* schemeName(Scheme s);  // "none" "dual" "loader"
// Why None, or a warning about the layout ("" none).
const char* schemeNote();
// The loader's version (the factory app's description), "" when none.
const char* loaderVersion();
// The loader was built with the dev signing key: its version carries
// "devkey" (the loader's build sets it), so any image signed with that
// key, or none, is taken. A warning on the pages, never a refusal.
bool devKey();
// Loader scheme: the app slot, the largest image. Else 0.
uint32_t appSlotSize();

// "Is now safe?" for a pull entry (auto-update, mothership): false with
// why keeps the running app. Set at setup; called on the task that asks
// (the update's, never the loop), so a read of the consumer's state.
// Without one: always safe.
using ReadyCheck = std::function<bool(String& why)>;
void setReadyCheck(ReadyCheck check);
bool ready(String& why);

// Handoffs. The station's Wi-Fi goes along (the shared copy when it is
// not up), the loader's AP password and the device id too.
// adminPassword: the loader's upload page asks for it, checked as a PBKDF2
// record made here (SecurityHash.h's salt size and rounds; "" or null: no
// password, the page warns).
void makePush(ota::Handoff& h, const char* adminPassword);
// The upload page's check from a user's stored record (SecurityManager's
// User: password = hex PBKDF2-SHA256, salt = hex salt). false: not such a
// record (a legacy row, a machine token): h keeps no password check.
bool setAdmin(ota::Handoff& h, const char* user, const char* saltHex, const char* hashHex);
// size: the image's bytes or the whole signed file's (a server's
// Content-Length), 0 unknown: the loader takes either. sha256Hex / sigHex:
// as the server gave them, over the image bytes (null or "": not given; a
// signed file's trailer may carry the signature, taken by a dev-key
// loader only: a production one checks it before its first write, so it
// needs both here). token: the same on every retry of the loader, so the
// server keeps it valid up to the install. false: why (bad hex, url too
// long, image larger than the app slot, no signature for a production
// loader).
bool makePull(ota::Handoff& h, const char* url, uint32_t size, const char* sha256Hex, const char* sigHex,
              const char* token, const char* targetVersion, String& why);

// Whether enter(h) would go now: the scheme, the record, flash writes
// allowed (PowerGuard), no other update under way, no restart claimed.
bool canEnter(const ota::Handoff& h, String& why);
// Restarts into the loader with h (restartDevice: never returns), or false
// with why. Any task; off the loop it parks the caller up to the reset
// (restartDevice). From async_tcp answer first: canEnter(), send, then
// enter() from the request's onDisconnect (as system.restart does).
bool enter(const ota::Handoff& h, const char* reason, String& why);

// Framework plumbing. begin(): reads the loader's result once and erases
// it (BootGuard::begin, before any module). fromLoader(): this boot came
// from the loader (it left a result).
void begin();
bool fromLoader();
// The loader's last result this boot; false: none.
bool lastResult(ota::Result& out);
// One line of it for a page ("" none).
String lastResultText();

// The loader's fallback AP (it opens one when it has no station): its
// name "esprack-loader-XXXX" (XXXX: the soft-AP MAC's last two bytes) and
// the WPA2 password the next handoff gives it: this device's own AP
// password when its AP runs with one (deviceAp), else the one generated
// for this device (begin(), NVS). false: no password (NVS failed): the
// loader's AP would be open (it warns). BootGuard's crash-loop entry
// gives none either (this device's AP password isn't known that early):
// open, so the owner reaches the loader without a cable.
bool loaderAp(String& ssid, String& pass, bool& deviceAp);

// The security manager the admin action asks who the requesting admin is
// (their stored record guards the loader's page). The upload page's
// module sets it; without one only a password given with the action does.
void setSecurityManager(SecurityManager* sm);
// Admin action system.enterUpdateMode (Loader scheme only): a push
// handoff; the loader's page asks for the requesting admin's password, or
// for the optional form field "password" when one is given.
void registerManifest(WebManager* web);

}  // namespace OtaLoader
}  // namespace ESPRack

#endif  // ESPRACK_SYSTEM_OTA_LOADER_H
