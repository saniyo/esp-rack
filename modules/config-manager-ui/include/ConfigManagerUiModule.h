#pragma once
#ifndef ESPRACK_CONFIG_MANAGER_UI_MODULE_H
#define ESPRACK_CONFIG_MANAGER_UI_MODULE_H

#include <Module.h>
#include "ConfigManagerService.h"
#include <memory>

class ConfigManagerUiModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "configManagerUi"; d.version = "1.0.0"; d.priority = 85;
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new ConfigManagerService(ctx.server, ctx.security, ctx.cfgMgr));
    svc_->registerManifest(ctx.web);
  }
 private:
  std::unique_ptr<ConfigManagerService> svc_;
};

#endif
