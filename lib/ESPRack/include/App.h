#pragma once
#ifndef ESPRACK_APP_H
#define ESPRACK_APP_H

// App — the running framework. Built by ESPRack::Builder, owns the
// installed Modules + framework singletons (ConfigManager, WebManager,
// WsManager, etc.) and drives the lifecycle.
//
// Consumer flow:
//
//   AsyncWebServer server(80);
//   std::unique_ptr<ESPRack::App> app;
//
//   void setup() {
//     app = ESPRack::Builder(&server, "MyDevice", "v1.0.0")
//       .core()
//       .install<MyModule>(args)
//       .build();
//     app->begin();
//   }
//
//   void loop() { app->loop(); }
//
// app->loop() blocks (HGW-LIB-003: no busy loop): after its own work and
// every module's onLoop() it waits for the nearest deadline
// (Module::loopWaitMs, ESPRACK_LOOP_MAX_WAIT_MS at most) or a wakeLoop(),
// so the loop task leaves the CPU to others (IDLE too, single-core C3/C6
// included). Code of your own beside app->loop() runs at the same pace: put
// it in a module with its loopWaitMs(), or call wakeLoop() when it has work.
//
// App is NOT a singleton — multiple instances are unusual but legal.
// Its destructor calls shutdown() if begin() ran.

#include <Arduino.h>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "Module.h"
#include "ConfigManager.h"
#include "WsManager.h"

// The restart path writes the config saves the write gate put off; each
// waits this long for a pause at most, then goes anyway.
#ifndef ESPRACK_SHUTDOWN_WRITE_WAIT_MS
#define ESPRACK_SHUTDOWN_WRITE_WAIT_MS 1000
#endif
#include "WebManager.h"

class AsyncWebServer;
class ITelegramProvider;   // forward — populated by TelegramModule
class IMqttProvider;       // forward — populated by MqttModule
class ITLSProvider;        // forward — populated by core App ctor
class TLSContextService;   // forward — concrete impl, owned by App
class ICertProvider;                // forward — populated by CertManagerModule
class IMothershipProvider;          // forward — populated by MothershipModule
class IMothershipProfileProvider;   // forward — populated by MothershipModule
class IWireguardProvider;           // forward — populated by WireGuardModule

namespace ESPRack {

class ILocationProvider;   // forward — populated by a location module (geo/ILocationProvider.h)
class IBleProvider;        // forward — populated by BleModule (ble/IBleProvider.h)
class IHistoryProvider;    // forward — populated by HistoryModule (history/IHistory.h)
class HwRegistry;          // hw/HwRegistry.h — owned by App

// Graceful device restart (LIB-FIX-006) — THE way to reboot. Runs every
// installed module's onShutdown() in reverse install order on the loop task
// (so it never races that module's onLoop), each timed against
// ESPRACK_SHUTDOWN_BUDGET_MS, then esp_restart(). Callable from any task:
// off-loop callers hand the restart to App::loop() and fall back to running
// it themselves if the loop doesn't respond within 3 s. Without an App it
// is a plain esp_restart(). Never returns.
//
// Deadline: the chip resets within ESPRACK_RESTART_DEADLINE_MS (30 s, or the
// hooks' total budget + 5 s when that is longer) whatever a hook does, and
// within it again once afterShutdown starts. A hook that hangs (the loop
// watchdog is paused for them) can't keep the device down. Also for a reset
// that bypasses restartDevice (esp_restart from a library).
//
// `afterShutdown` (optional) runs once, AFTER every onShutdown() and right
// before the chip resets — e.g. factory reset wipes /config there, so a
// module that persists state in onShutdown() can't resurrect the files.
// It runs inside one write gate slot (power/WriteGate.h; a wait of
// ESPRACK_SHUTDOWN_WRITE_WAIT_MS at most for a pause, then it goes), after
// the config saves the gate put off. Callers refuse the request while
// PowerGuard pauses writes (PowerGuard::writesAllowed()); a step already
// confirmed still runs if writes were paused since (and logs it).
[[noreturn]] void restartDevice(const char* reason,
                                std::function<void()> afterShutdown = nullptr);

// Reason passed to the restart in progress ("" when none). Lets an
// onShutdown() skip work that makes no sense for that reason.
const char* restartReason();

// Why THIS boot happened, stable for the whole boot:
//   * a software reset through restartDevice() -> the reason it was given
//     (restartDevice stores it in RTC memory before the hooks run);
//   * a software reset that bypassed it (esp_restart, a library) -> "idf";
//   * otherwise the hardware reason: "power-on", "ext", "panic", "int-wdt",
//     "task-wdt", "wdt", "brownout", "deep-sleep", "usb", "jtag", "unknown".
const char* lastRestartReason();

// The loop task's wait (App::loop) ends now: call it where you set a flag
// that an onLoop() reacts to. Any task; wakeLoopFromISR() from an ISR. Cheap
// (a semaphore give, no allocation), safe before App::begin and after
// shutdown (no-op while no App was built). Wakes coalesce.
void wakeLoop();
void wakeLoopFromISR();

// Reasons used by the built-in modules (≤ 23 chars survive the reboot).
static constexpr const char* kRestartUser          = "user";          // system.restart
static constexpr const char* kRestartUpload        = "upload";        // firmware upload
static constexpr const char* kRestartConfigRestore = "config-restore";
static constexpr const char* kRestartFactoryReset  = "system.factoryReset";
static constexpr const char* kRestartAutoUpdate    = "auto-update";
static constexpr const char* kRestartMothership    = "mothership";    // mothership reboot command
static constexpr const char* kRestartOta           = "ota";           // ArduinoOTA
static constexpr const char* kRestartHwRestore     = "hw-restore";    // System -> Hardware

class App {
 public:
  App(AsyncWebServer* server,
      const char* deviceName,
      const char* deviceVersion);
  ~App();

  // Lifecycle — see Module.h for phase semantics. Each method is
  // idempotent: calling begin() twice is a no-op after the first.
  void begin();
  void loop();
  void shutdown();

  // Framework singleton access. Modules normally receive these via
  // ModuleContext during onInstall — these getters are for the rare
  // consumer that needs to reach into the framework from main loop
  // code (e.g. broadcasting a custom WS message from a non-module
  // origin).
  AsyncWebServer*    server()        { return server_; }
  ConfigManager*     configManager() { return &configManager_; }
  WebManager*        webManager()    { return &webManager_; }
  WsManager*         wsManager()     { return &wsManager_; }
  SecurityManager*   security()      { return security_; }

  // Allows the Security module to register its SecurityManager
  // implementation pointer once installed. Builder invokes this from
  // SecurityModule::onInstall(). External consumers should not call.
  void setSecurityManager(SecurityManager* sm) { security_ = sm; }

  // PresenceService late-binding — set by PresenceModule during its
  // onInstall. Subsequent modules' install order picks up the real
  // pointer via Builder's per-step ctx refresh.
  PresenceService* presence()                  { return presence_; }
  void             setPresence(PresenceService* p) { presence_ = p; }

  // ITelegramProvider late-binding — populated by TelegramModule's
  // onInstall via setTelegram(svc.get()). Consuming services that
  // want to stream into Telegram subscribe through this pointer in
  // their onBegin(). Returns nullptr when TelegramModule wasn't
  // installed; consumer code must null-check (matches the optional-
  // dependency pattern used for security/presence).
  ITelegramProvider* telegram()                  { return telegram_; }
  void               setTelegram(ITelegramProvider* p) { telegram_ = p; }

  // IMqttProvider late-binding — same pattern as telegram(). Set by
  // MqttModule::onInstall. Consumers that want to publish/subscribe
  // through the broker pull through this pointer in their onBegin.
  IMqttProvider*     mqtt()                  { return mqtt_; }
  void               setMqtt(IMqttProvider* p) { mqtt_ = p; }

  // ITLSProvider — framework-side TLS context primitive. Owned by
  // the App itself (singleton), instantiated in ctor (Phase 1.1+).
  // Consumer modules pull it for their HTTPS clients, e.g.
  // `app->tls()->attachToClient(client)`. Set via setTls() so the
  // App ctor can defer TLSContextService construction past static
  // init order issues.
  ITLSProvider*      tls()                   { return tls_; }
  void               setTls(ITLSProvider* p) { tls_ = p; }

  // ICertProvider late-binding — same pattern as mqtt(). Set by
  // CertManagerModule::onInstall when that module is installed. Consumer
  // services check `app->cert()->hasValidCert()` before assuming
  // mTLS is available.
  ICertProvider*     cert()                  { return cert_; }
  void               setCert(ICertProvider* p) { cert_ = p; }

  // IMothershipProvider late-binding — populated by MothershipModule.
  // Phase 3 (WireGuard) reads check-in state to decide when to bring
  // up tunnels; status panels read for live UI.
  IMothershipProvider* mothership()                  { return mothership_; }
  void                 setMothership(IMothershipProvider* p) { mothership_ = p; }

  // IMothershipProfileProvider late-binding — populated by
  // MothershipModule (same module, separate interface — same service
  // implements both). CertManagerService reads enroll/recover URLs
  // through this provider at request time instead of carrying its
  // own URL fields, so a profile switch in Mothership UI applies
  // to PKI flows transparently. Null when MothershipModule isn't
  // installed — consumers must guard accordingly.
  IMothershipProfileProvider* mothershipProfile()                  { return mothershipProfile_; }
  void                        setMothershipProfile(IMothershipProfileProvider* p) { mothershipProfile_ = p; }

  // IWireguardProvider late-binding — populated by WireGuardModule.
  // MothershipService::actionOpenTunnel / actionCloseTunnel call into
  // this to bring the on-demand tunnel up/down.
  IWireguardProvider* wireguard()                    { return wireguard_; }
  void                setWireguard(IWireguardProvider* p) { wireguard_ = p; }

  // ILocationProvider late-binding — populated by a location module. A GPS
  // module feeds fixes in through it; consumers (astro, status) read it.
  // Null when no location module is installed.
  ILocationProvider*  location()                        { return location_; }
  void                setLocation(ILocationProvider* p) { location_ = p; }

  // IBleProvider late-binding — populated by BleModule, the only owner of
  // the BLE host stack: scans and central links for any module that talks
  // to a BLE peripheral. Null when BleModule is not installed.
  IBleProvider*       ble()                             { return ble_; }
  void                setBle(IBleProvider* p)           { ble_ = p; }
  // IHistoryProvider late-binding — populated by HistoryModule, the only
  // owner of the history files (docs/plans/history.md). null: not installed.
  IHistoryProvider*   history()                         { return history_; }
  void                setHistory(IHistoryProvider* p)   { history_ = p; }

  // Hardware registry (LIB-FEAT-001, hw/HwRegistry.h): reserved pins,
  // external HW module instances and their claims, find<I>(id) /
  // forEach<I>(fn) lookup. Always present.
  HwRegistry*         hw()                              { return hw_.get(); }

  // Any installed module by its descriptor id (an external HW instance by
  // its instance id), or nullptr. Walks the modules and asks describe(), so
  // look up once (onBegin) and keep the pointer.
  Module* findModule(const char* moduleId) const;
  // A module by id, as interface I (nullptr when there's no such module or
  // it doesn't implement I): app->find<IMyBus>("myBus").
  template <class I>
  I* find(const char* moduleId) const {
    Module* m = findModule(moduleId);
    return m ? static_cast<I*>(m->queryInterface(I::kIfaceId)) : nullptr;
  }

  // Read-only device identity — useful for AutoUpdateModule etc. that
  // pass these to their underlying service ctor. Set once at App
  // construction.
  const char* deviceName()    const { return deviceName_.c_str();    }
  const char* deviceVersion() const { return deviceVersion_.c_str(); }

 private:
  // Builder is the sole authorised constructor of populated App
  // instances. It transfers the `modules_` vector into the App via
  // adoptModules() then disowns it.
  friend class Builder;
  friend void restartDevice(const char* reason, std::function<void()> afterShutdown);
  friend const char* restartReason();
  void adoptModules(std::vector<std::unique_ptr<Module>>&& modules);

  // Restart plumbing (LIB-FIX-006). runShutdownOnce() is idempotent across
  // tasks: whichever path gets there first (loop, fallback, esp shutdown
  // handler) runs the module hooks; the others skip them. Returns true when
  // this call ran (or had nothing to run) the hooks, false when another task
  // owns them — see hooksDone_.
  bool runShutdownOnce(const char* reason);
  [[noreturn]] void finishAndRestart();    // hooks -> afterShutdown -> esp_restart
  void runAfterShutdownOnce();             // runs it, or waits for the task that does
  bool waitHooks(uint32_t ms) const;       // true when the hooks finished in time
  uint32_t loopWaitMs() const;             // App::loop's wait: modules + the core's deadlines
  uint32_t shutdownGraceMs() const;
  uint32_t restartDeadlineMs() const;      // the hooks' share of the deadline
  static void onEspShutdown();             // esp_register_shutdown_handler hook
  std::function<void()> afterShutdown_;
  std::atomic<uint8_t> afterState_ {0};    // 0 not started, 1 running, 2 done
  std::atomic<bool> shutdownDone_ {false}; // the hooks have been claimed
  std::atomic<bool> hooksDone_ {false};    // ... and have finished
  std::atomic<bool> abandonHooks_ {false}; // the caller took over: stop between hooks
  std::atomic<bool> restartRequested_ {false};
  std::atomic<bool> restartClaimed_ {false};   // first restartDevice() caller wins
  const char*       restartReason_ {"unknown"};
  void*             loopTask_ {nullptr};   // TaskHandle_t of the App::loop caller

  AsyncWebServer*  server_ {nullptr};
  String           deviceName_;
  String           deviceVersion_;

  ConfigManager    configManager_;
  WsManager        wsManager_;
  WebManager       webManager_;
  SecurityManager*    security_ {nullptr};   // populated by SecurityModule
  PresenceService*    presence_ {nullptr};   // populated by PresenceModule
  ITelegramProvider*  telegram_ {nullptr};   // populated by TelegramModule
  IMqttProvider*      mqtt_     {nullptr};   // populated by MqttModule
  // TLS context is App-owned (not module-supplied) because it's a
  // framework primitive every HTTPS-using module needs. unique_ptr +
  // forward decl keeps App.h light — only App.cpp needs the heavy
  // TLSContextService.h pull.
  std::unique_ptr<TLSContextService> tlsContext_;
  ITLSProvider*       tls_      {nullptr};   // points at tlsContext_.get()
  ICertProvider*      cert_     {nullptr};   // populated by CertManagerModule
  IMothershipProvider* mothership_ {nullptr}; // populated by MothershipModule
  IMothershipProfileProvider* mothershipProfile_ {nullptr};  // ditto
  IWireguardProvider* wireguard_ {nullptr};  // populated by WireGuardModule
  ILocationProvider*  location_  {nullptr};  // populated by a location module
  IBleProvider*       ble_       {nullptr};  // populated by BleModule
  IHistoryProvider*   history_   {nullptr};  // populated by HistoryModule
  std::unique_ptr<HwRegistry> hw_;           // created in the ctor

  std::vector<std::unique_ptr<Module>> modules_;
  bool             begun_ {false};
};

}  // namespace ESPRack

#endif  // ESPRACK_APP_H
