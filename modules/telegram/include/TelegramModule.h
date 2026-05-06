#pragma once
#ifndef ESPRACK_TELEGRAM_MODULE_H
#define ESPRACK_TELEGRAM_MODULE_H

#include <Module.h>
#include "TelegramService.h"
#include <memory>

class TelegramModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "telegram"; d.version = "1.0.0"; d.priority = 40;
    d.requires_ = {"wifi"};
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new TelegramService(ctx.cfgMgr));
    svc_->registerManifest(ctx.web);
  }
  void onBegin() override { if (svc_) svc_->begin(); }
 private:
  std::unique_ptr<TelegramService> svc_;
};

#endif
