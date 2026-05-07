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
// App is NOT a singleton — multiple instances are unusual but legal.
// Its destructor calls shutdown() if begin() ran.

#include <Arduino.h>
#include <memory>
#include <vector>

#include "Module.h"
#include "ConfigManager.h"
#include "WsManager.h"
#include "WebManager.h"

class AsyncWebServer;
class ITelegramProvider;   // forward — populated by TelegramModule
class IMqttDispatcher;     // forward — populated by MqttModule (TBD subscription model)

namespace ESPRack {

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
  void adoptModules(std::vector<std::unique_ptr<Module>>&& modules);

  AsyncWebServer*  server_ {nullptr};
  String           deviceName_;
  String           deviceVersion_;

  ConfigManager    configManager_;
  WsManager        wsManager_;
  WebManager       webManager_;
  SecurityManager*    security_ {nullptr};   // populated by SecurityModule
  PresenceService*    presence_ {nullptr};   // populated by PresenceModule
  ITelegramProvider*  telegram_ {nullptr};   // populated by TelegramModule

  std::vector<std::unique_ptr<Module>> modules_;
  bool             begun_ {false};
};

}  // namespace ESPRack

#endif  // ESPRACK_APP_H
