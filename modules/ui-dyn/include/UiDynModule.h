#pragma once
#ifndef ESPRACK_UI_DYN_MODULE_H
#define ESPRACK_UI_DYN_MODULE_H

// UiDynModule — serves /rest/uiManifest, the React app's source of truth
// for menu / routes / tabs. Wraps the legacy UiDynService.

#include <Module.h>
#include "UiDynService.h"
#include <memory>

class UiDynModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "uiDyn"; d.version = "1.0.0"; d.priority = 6;
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new UiDynService(ctx.server, ctx.security));
  }
  void onBegin() override { if (svc_) svc_->begin(); }
 private:
  std::unique_ptr<UiDynService> svc_;
};

#endif
