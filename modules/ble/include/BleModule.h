#pragma once
#ifndef ESPRACK_BLE_MODULE_H
#define ESPRACK_BLE_MODULE_H

// BleModule — the chip's Bluetooth LE radio as a framework service.
//
//   .install<BleModule>()
//
// Internal module (like wifi): one instance, id "ble". It is the only owner
// of the BLE host stack and publishes ESPRack::IBleProvider through
// App::ble() in onInstall, so any module installed after it (priority > 12)
// can register links in its own onBegin.
//
// The stack is NimBLE-Arduino 2.x; the consumer lists it in lib_deps
// (h2zero/NimBLE-Arduino @ 2.5.1), like the other third-party libraries.
// Without it the module still builds: the page says the stack is missing
// and registerLink() refuses.
//
// Blocking stack calls (connect, service discovery, subscribe) run in the
// module's own FreeRTOS task, never on the loop task (LoopWdt). In BootGuard
// safe mode the radio stays off.

#include <App.h>
#include <Module.h>

#include <memory>

#include "BleService.h"

class BleModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "ble";
    d.version = BLE_MODULE_VERSION;
    d.priority = 12;
  }

  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new BleService(ctx.cfgMgr));
    // Late-bind before any later module's onInstall/onBegin asks for it.
    if (ctx.app) ctx.app->setBle(svc_.get());
    svc_->registerManifest(ctx.web);
  }

  void onBegin() override { if (svc_) svc_->begin(); }
  void onLoop() override { if (svc_) svc_->loop(); }
  // Disconnect every link and stop scanning before a restart.
  void onShutdown() override { if (svc_) svc_->end(); }

  BleService* service() { return svc_.get(); }

 private:
  std::unique_ptr<BleService> svc_;
};

#endif  // ESPRACK_BLE_MODULE_H
