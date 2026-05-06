#pragma once
#ifndef ESPRACK_FACTORY_RESET_MODULE_H
#define ESPRACK_FACTORY_RESET_MODULE_H

#include <Module.h>
#include <ESPFS.h>
#include "FactoryResetService.h"
#include <memory>

class FactoryResetModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "factoryReset"; d.version = "1.0.0"; d.priority = 90;
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new FactoryResetService(ctx.server, &ESPFS, ctx.security));
    svc_->registerManifest(ctx.web);
  }
 private:
  std::unique_ptr<FactoryResetService> svc_;
};

#endif
