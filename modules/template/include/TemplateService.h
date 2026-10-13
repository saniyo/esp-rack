#pragma once
#ifndef ESPRACK_TEMPLATE_SERVICE_H
#define ESPRACK_TEMPLATE_SERVICE_H

// TemplateService — the working half of TemplateModule. Shows the four
// pieces almost every module needs:
//
//   * TemplateState            — ONE struct holding persisted settings AND
//                                runtime values, guarded by the
//                                StatefulService<T> recursive mutex
//   * ConfigDelegate<T> _cfg   — persistence to /config/template.json
//   * WebFeatureEntry<T>*      — UI feature: form GET, state GET/POST,
//                                live WS channel (all paths derived from id)
//   * action + custom endpoint — template.resetCounters and
//                                GET /api/v1/template/lines
//
// Threading: onLoop() runs on loopTask; REST handlers and actions run on
// the async_tcp task; WS writes are applied by WsManager::processAllQueues,
// which runs on BOTH (App::loop and the WS event handler). Anything shared
// lives in _state and is touched only through read()/update*(). Hardware
// (the UART) is touched ONLY from loop(); web-side changes raise a flag
// that loop() consumes.

#include <Arduino.h>
#include <HardwareSerial.h>
#include <atomic>

#include <ConfigDelegate.h>
#include <FormBuilder.h>          // FormBuilder::updateValue — POST parse-back
#include <StatefulService.h>
#include <WebFeatureDelegate.h>   // WebFeatureEntry<T>
#include <web/Web.h>              // web::Form fields + WebFeature/WebTab/WebAction

class WebManager;
class AsyncWebServerRequest;

// Factory defaults. The consumer overrides them in factory_settings.ini
// (-D FACTORY_TEMPLATE_RX_PIN=16 ...); the UI can change them at runtime.
#ifndef FACTORY_TEMPLATE_ENABLED
#define FACTORY_TEMPLATE_ENABLED false
#endif
#ifndef FACTORY_TEMPLATE_RX_PIN
#define FACTORY_TEMPLATE_RX_PIN -1   // -1 = keep the UART's board-default pin
#endif
#ifndef FACTORY_TEMPLATE_TX_PIN
#define FACTORY_TEMPLATE_TX_PIN -1
#endif
#ifndef FACTORY_TEMPLATE_BAUD
#define FACTORY_TEMPLATE_BAUD 9600
#endif

#define TEMPLATE_CONFIG_FILE "/config/template.json"
#define TEMPLATE_LINES_PATH  "/api/v1/template/lines"

class TemplateState {
 public:
  // ---- persisted settings (readConfig writes exactly these) ----
  // Field types matter for parse-back: FormBuilder::updateValue has
  // coercing overloads for bool/uint8_t/uint16_t/int/float/double; other
  // types go through a strict template that ignores numeric STRINGS.
  bool     enabled = FACTORY_TEMPLATE_ENABLED;
  int      rxPin   = FACTORY_TEMPLATE_RX_PIN;
  int      txPin   = FACTORY_TEMPLATE_TX_PIN;
  int      baud    = FACTORY_TEMPLATE_BAUD;
  uint16_t pushMs  = 1000;   // live WS push period

  // ---- runtime (never persisted) ----
  static constexpr size_t kRing    = 8;    // recent-lines ring for /lines
  static constexpr size_t kLineLen = 64;
  bool     portOpen     = false;
  uint32_t bytesRx      = 0;
  uint32_t linesRx      = 0;
  uint32_t bytesAtSample = 0;  // bytesRx at the previous rate sample
  float    rateBps      = 0;
  uint32_t sampleTs     = 0;   // millis() of the last rate sample
  char     ring[kRing][kLineLen] = {};

  const char* lastLine() const { return linesRx ? ring[(linesRx - 1) % kRing] : ""; }

  // File <-> state. readConfig is what lands on flash; update parses the
  // file AND every write path (UI save, POST /state, WS write).
  static void readConfig(TemplateState& s, JsonObject& root);
  static StateUpdateResult update(JsonObject& root, TemplateState& s);

  // GET /api/v1/template/form — one web::Form per tab key.
  static void buildForm(TemplateState& s, JsonObject& root);

  // Flat values: every WS push AND GET /api/v1/template/state.
  static void readState(TemplateState& s, JsonObject& root);
};

class TemplateService : public StatefulService<TemplateState> {
 public:
  TemplateService(ConfigManager* cfgMgr, HardwareSerial& port);

  void registerManifest(WebManager* web);  // from onInstall
  void begin();                            // from onBegin
  void loop();                             // from onLoop — never blocks
  void end();                              // from onShutdown

 private:
  void applyPort();   // (re)open the UART from current settings — loop() only
  void drainPort();   // bounded non-blocking read
  void pushLive();    // cadence-gated WS push, only while someone watches
  void serveLines(AsyncWebServerRequest* r);

  HardwareSerial&                 _port;
  ConfigDelegate<TemplateState>   _cfg;
  WebFeatureEntry<TemplateState>* _feature{nullptr};
  std::atomic<bool>               _applyPending{false};

  // UART-side bookkeeping — loop() only, so no lock needed.
  bool     _open{false};
  int      _openRx{-1}, _openTx{-1}, _openBaud{0};
  char     _line[TemplateState::kLineLen]{};
  size_t   _lineLen{0};
  uint32_t _lastPushMs{0};
};

#endif  // ESPRACK_TEMPLATE_SERVICE_H
