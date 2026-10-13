#pragma once
#ifndef ESPRACK_UI_DYN_MODULE_H
#define ESPRACK_UI_DYN_MODULE_H

// UiDynModule — serves /api/v1/ui/dynamic, the React app's source of truth
// for menu / routes / tabs. Wraps UiDynService.

#include <Module.h>
#include <App.h>
#include "UiDynService.h"
#include <memory>

class UiDynModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "uiDyn"; d.version = "1.0.0"; d.priority = 6;
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    app_ = ctx.app;
    svc_.reset(new UiDynService(ctx.server, ctx.security));
    // Phase 7c — register the manifest GET as a proxy endpoint so
    // the mship-ui reverse proxy (Phase 7d ws-bridge) can reach it.
    // Without this the React app stuck on "Connecting to device..."
    // because /api/v1/ui/dynamic 404'd through the proxy.
    if (svc_ && ctx.web) svc_->registerProxy(ctx.web);
  }
  void onBegin() override {
    if (!svc_) return;
    // Every onInstall has run by now: app->security() is the real manager
    // when SecurityModule is installed (LIB-FIX-005).
    if (app_) svc_->setSecurityManager(app_->security());
    svc_->begin();
  }
 private:
  ESPRack::App* app_{nullptr};
  std::unique_ptr<UiDynService> svc_;
};

#endif
