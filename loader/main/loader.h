#pragma once
#ifndef ESPRACK_LOADER_H
#define ESPRACK_LOADER_H

// esprack-loader: the small ESP-IDF app in the `factory` partition of the
// `loader` OTA scheme (docs/plans/lib-ota-loader.md). The device's app
// lives in the one slot ota_0; to update it the app writes a handoff
// (system/OtaHandoff.h), sets the boot partition to factory and restarts
// into this loader, which
//   - joins Wi-Fi (the handoff's station, else the one it keeps in IDF's
//     storage; no station: an AP "esprack-loader-XXXX", WPA2 with the
//     handoff's AP password, else open);
//   - pull: downloads the image (plain HTTP, http_lite.h); push, or no
//     handoff (the boot button: the bootloader's factory-reset pin, a
//     build choice): serves a minimal upload page (esp_http_server, the
//     admin's password checked as the app stores it: PBKDF2);
//   - writes it into ota_0 in 4 KB sector steps (FlashSteps), hashing it
//     on the fly, checks header, chip, SHA-256 and the ECDSA P-256
//     signature (ota_pubkey.h), sets ota_0 to boot and restarts;
//   - writes a Result before every restart into the app.
// Before the first flash write (the point of no return) any failure boots
// the old app again; after it the loader keeps trying (pull: 10 s, 30 s,
// 2 min, then every 5 min for good, past the handoff's attempts its page
// says the update is stuck; an upload works all along). With a production
// key a pull needs the handoff's SHA-256 and signature, checked before
// the first write. ota_0 boots only a whole, checked image (the image's
// head goes in last, image_writer.h): a power cut lands back here. It
// never writes its own partition. The same code on every chip.
//
// Tasks: app_main runs the session (join, pull, waits); the HTTP server's
// task takes uploads. One image at a time (Session::writer).

#include <stdarg.h>
#include <stdint.h>

#include <atomic>

#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

#include "system/OtaHandoff.h"

#ifndef LOADER_JOIN_MS
#define LOADER_JOIN_MS 30000  // a station's join, each time
#endif
#ifndef LOADER_IDLE_RETURN_MS
#define LOADER_IDLE_RETURN_MS (10 * 60 * 1000)  // push mode: no upload this long -> the app again
#endif

namespace loader {

namespace ota = ESPRack::ota;

enum class Phase : uint8_t {
  Start,
  Joining,     // the station
  Waiting,     // for an upload
  Pulling,     // a download under way
  Pausing,     // between downloads (after the point of no return)
  Uploading,   // an upload under way
  Activating,  // the image is in: boot it
  Stopped      // nothing to do (no app slot)
};
const char* phaseName(Phase p);

// Session::events
constexpr EventBits_t kEvDone = BIT0;         // an image is in and checked: activate it
constexpr EventBits_t kEvBack = BIT1;         // the page asked to boot the app again
constexpr EventBits_t kEvUploadStart = BIT2;  // an upload took the writer
constexpr EventBits_t kEvUploadEnd = BIT3;    // it failed and gave the writer back
constexpr EventBits_t kEvNetUp = BIT4;        // a level, not an event: the station has an address

struct Session {
  ota::Handoff handoff = {};
  bool haveHandoff = false;
  ota::Result result = {};  // goes to the app before every restart into it
  const esp_partition_t* slot = nullptr;  // ota_0
  bool appValid = false;  // ota_0 holds an image that boots (false once written to)
  char appVersion[32] = {};
  char newVersion[32] = {};  // the image written (kEvDone)
  // An image is in and checked (set under Session::writer, with kEvDone):
  // no other one goes over it until it boots or its activation failed.
  std::atomic<bool> imageIn{false};
  bool prodKey = false;  // a production signing key: signatures mandatory
  uint32_t priorMs = 0;  // in the loader before this boot (the same update)
  EventGroupHandle_t events = nullptr;
  SemaphoreHandle_t writer = nullptr;  // one image at a time: the pull or an upload
  SemaphoreHandle_t lock = nullptr;    // the texts below
  std::atomic<uint8_t> phase{0};
  std::atomic<uint32_t> bytes{0}, total{0};
  char error[64] = {};     // the last failure
  char warning[96] = {};   // e.g. an unsigned image went in (dev key)
};
extern Session g;

void setPhase(Phase p);
Phase phase();
// The last failure / a warning: the page, the log, the Result.
void setError(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void setWarning(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void copyTexts(char* error, size_t errorCap, char* warning, size_t warningCap);
uint32_t sessionMs();  // in the loader, this update (all boots)

// The image signing key (ota_pubkey.h).
const uint8_t* pubKey();
const char* keyId();

// Net (loader_net.cpp).
namespace net {
// The radio and the network stack; nvs: IDF's Wi-Fi storage may be used
// (the kept station). false: no Wi-Fi.
bool init(bool nvs);
// The station: ssid/pass from the handoff (kept in IDF's storage for a
// later entry), or with ssid "" the kept one. Retries in the background;
// waits up to timeoutMs for an address. false: none configured, or no
// address in time.
bool join(const char* ssid, const char* pass, uint32_t timeoutMs);
bool waitUp(uint32_t timeoutMs);
bool up();
bool configured();
// The fallback AP "esprack-loader-XXXX": WPA2 with the handoff's apPass,
// open without one (the page asks for the admin password either way).
// Next to the station when one is configured.
void startAp();
bool apOn();
bool apOpen();  // on, without a password
const char* apName();
}  // namespace net

// The HTTP server: the page, /status, /upload, /back (loader_web.cpp).
bool webStart();
void webStop();

// One download of the handoff's image into ota_0 (loader_pull.cpp).
enum class Pull : uint8_t {
  Ok,      // the image is in and checked
  Before,  // failed before the first flash write: ota_0 untouched
  After    // failed after it: ota_0 no longer boots
};
Pull pullOnce();

}  // namespace loader

#endif  // ESPRACK_LOADER_H
