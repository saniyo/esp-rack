#pragma once
#ifndef ESPRACK_WEB_ENDPOINTS_MODULE_H
#define ESPRACK_WEB_ENDPOINTS_MODULE_H

#include <Module.h>
#include "WebEndpointsService.h"
#include <memory>

class WebEndpointsModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "webEndpoints"; d.version = "1.0.0"; d.priority = 85;
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new WebEndpointsService(ctx.server, ctx.security, ctx.web, ctx.ws));
    svc_->registerManifest(ctx.web);
  }
 private:
  std::unique_ptr<WebEndpointsService> svc_;
};

#endif
