#pragma once
#ifndef ESPRACK_SYSTEM_STATUS_MODULE_H
#define ESPRACK_SYSTEM_STATUS_MODULE_H

#include <Module.h>
#include "SystemStatus.h"
#include <memory>

class SystemStatusModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "systemStatus"; d.version = "1.0.0"; d.priority = 80;
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new SystemStatus(ctx.server, ctx.security, ctx.web));
    svc_->registerManifest(ctx.web);
  }
 private:
  std::unique_ptr<SystemStatus> svc_;
};

#endif
