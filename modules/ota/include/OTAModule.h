#pragma once
#ifndef ESPRACK_OTA_MODULE_H
#define ESPRACK_OTA_MODULE_H

#include <Module.h>
#include "OTASettingsService.h"
#include <memory>

class OTAModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "ota"; d.version = "1.0.0"; d.priority = 40;
    d.requires_ = {"wifi"};
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new OTASettingsService(ctx.cfgMgr));
    svc_->registerManifest(ctx.web);
  }
  void onBegin() override { if (svc_) svc_->begin(); }
  void onLoop()  override { if (svc_) svc_->loop();  }
 private:
  std::unique_ptr<OTASettingsService> svc_;
};

#endif
