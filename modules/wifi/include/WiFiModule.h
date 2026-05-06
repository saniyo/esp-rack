#pragma once
#ifndef ESPRACK_WIFI_MODULE_H
#define ESPRACK_WIFI_MODULE_H

// WiFiModule — thin ESPRack::Module wrapper around WiFiSettingsService
// (the existing implementation copied verbatim from the legacy
// framework). The wrapper exists so consumers express WiFi as a
// plug-in via the standard Builder API:
//
//   ESPRack::Builder(server)
//     .install<WiFiModule>()
//     .build();
//
// Module ID: "wifi". Priority 10 (early — many other modules depend
// on having WiFi up before their own network-touching onBegin runs).

#include <Module.h>
#include "WiFiSettingsService.h"
#include <memory>

class WiFiModule : public ESPRack::Module {
 public:
  WiFiModule() = default;

  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id       = "wifi";
    d.version  = "1.0.0";
    d.priority = 10;
  }

  void onInstall(ESPRack::ModuleContext& ctx) override {
    // ConfigDelegate registration happens inside WiFiSettingsService
    // ctor — defer construction until we hold ctx.cfgMgr.
    svc_.reset(new WiFiSettingsService(ctx.cfgMgr));
    svc_->registerManifest(ctx.web);
  }

  void onBegin() override { if (svc_) svc_->begin(); }
  void onLoop()  override { if (svc_) svc_->loop();  }

  // Escape hatch: lets a module that explicitly depends on the WiFi
  // service reach into it. Most consumers shouldn't need this — the
  // STA/AP credentials live on disk, not in the C++ object.
  WiFiSettingsService* service() { return svc_.get(); }

 private:
  std::unique_ptr<WiFiSettingsService> svc_;
};

#endif  // ESPRACK_WIFI_MODULE_H
