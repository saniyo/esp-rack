#pragma once
#ifndef ESPRACK_WS_DIAG_MODULE_H
#define ESPRACK_WS_DIAG_MODULE_H

#include <Module.h>
#include "WsDiagService.h"
#include <memory>

class WsDiagModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "wsDiag"; d.version = "1.0.0"; d.priority = 80;
    d.requires_ = {"presence"};
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new WsDiagService(ctx.server, ctx.security, ctx.ws, ctx.presence));
    svc_->registerManifest(ctx.web);
  }
 private:
  std::unique_ptr<WsDiagService> svc_;
};

#endif
