#pragma once
#ifndef ESPRACK_RESTART_MODULE_H
#define ESPRACK_RESTART_MODULE_H

#include <Module.h>
#include "RestartService.h"
#include <memory>

class RestartModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "restart"; d.version = "1.0.0"; d.priority = 90;
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new RestartService(ctx.server, ctx.security));
    svc_->registerManifest(ctx.web);
  }
 private:
  std::unique_ptr<RestartService> svc_;
};

#endif
