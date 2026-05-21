#pragma once
#ifndef ESPRACK_AUTO_UPDATE_MODULE_H
#define ESPRACK_AUTO_UPDATE_MODULE_H

#include <Module.h>
#include "AutoUpdateService.h"
#include <memory>

class AutoUpdateModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "autoUpdate"; d.version = "1.0.0"; d.priority = 40;
    d.requires_ = {"wifi"};
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new AutoUpdateService(
        ctx.cfgMgr,
        ctx.deviceName    ? ctx.deviceName    : "",
        ctx.deviceVersion ? ctx.deviceVersion : ""));
    svc_->registerManifest(ctx.web);
  }
  void onBegin() override { if (svc_) svc_->begin(); }
  void onLoop()  override { if (svc_) svc_->loop();  }
 private:
  std::unique_ptr<AutoUpdateService> svc_;
};

#endif
