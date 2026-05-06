#pragma once
#ifndef ESPRACK_AP_MODULE_H
#define ESPRACK_AP_MODULE_H

// APModule — soft-AP for fallback / first-boot provisioning. Thin
// ESPRack::Module wrapper around the legacy APSettingsService.
//
// Usage:
//   ESPRack::Builder(server).install<APModule>().build();
//
// Module ID: "ap". Priority 10 (early — same band as WiFi STA so the
// radio is configured before downstream modules touch it).

#include <Module.h>
#include "APSettingsService.h"
#include <memory>

class APModule : public ESPRack::Module {
 public:
  APModule() = default;

  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id       = "ap";
    d.version  = "1.0.0";
    d.priority = 10;
  }

  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new APSettingsService(ctx.cfgMgr));
    svc_->registerManifest(ctx.web);
  }

  void onBegin() override { if (svc_) svc_->begin(); }
  void onLoop()  override { if (svc_) svc_->loop();  }

  APSettingsService* service() { return svc_.get(); }

 private:
  std::unique_ptr<APSettingsService> svc_;
};

#endif  // ESPRACK_AP_MODULE_H
