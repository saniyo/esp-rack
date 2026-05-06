#pragma once
#ifndef ESPRACK_NTP_MODULE_H
#define ESPRACK_NTP_MODULE_H

#include <Module.h>
#include "NTPSettingsService.h"
#include <memory>

class NTPModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "ntp"; d.version = "1.0.0"; d.priority = 30;
    d.requires_ = {"wifi"};
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new NTPSettingsService(ctx.cfgMgr));
    svc_->registerManifest(ctx.web);
  }
  void onBegin() override { if (svc_) svc_->begin(); }
  void onLoop()  override { if (svc_) svc_->loop();  }
 private:
  std::unique_ptr<NTPSettingsService> svc_;
};

#endif
