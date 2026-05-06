#pragma once
#ifndef ESPRACK_PRESENCE_MODULE_H
#define ESPRACK_PRESENCE_MODULE_H

// PresenceModule — tracks active client tabs (REST + WS). Late-binds
// its PresenceService into App so downstream modules (WsDiagModule,
// SecurityModule) receive it via ModuleContext.

#include <Module.h>
#include <App.h>
#include "PresenceService.h"
#include <memory>

class PresenceModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "presence"; d.version = "1.0.0"; d.priority = 7;
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new PresenceService());
    if (ctx.app) ctx.app->setPresence(svc_.get());
    svc_->mountWs(ctx.server, ctx.security);
  }
  void onLoop() override {
    if (!svc_) return;
    static uint32_t last = 0;
    uint32_t now = millis();
    if ((uint32_t)(now - last) >= 30000UL) {
      last = now;
      svc_->purgeStale();
    }
  }
 private:
  std::unique_ptr<PresenceService> svc_;
};

#endif
