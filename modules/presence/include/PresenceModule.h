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
    // Resolve per handshake: SecurityModule installs AFTER presence (it
    // requires it), so ctx.security here is still the Null manager.
    ESPRack::App* app = ctx.app;
    svc_->mountWs(ctx.server, [app]() -> SecurityManager* {
      return app ? app->security() : nullptr;
    });
  }
  void onLoop() override {
    if (!svc_) return;
    uint32_t now = millis();
    if ((uint32_t)(now - lastPurgeMs_) >= kPurgeMs) {
      lastPurgeMs_ = now;
      svc_->purgeStale();
    }
  }
  uint32_t loopWaitMs(uint32_t nowMs) const override {
    return svc_ ? ESPRack::LoopWait::dueIn(nowMs, lastPurgeMs_, kPurgeMs) : ESPRACK_LOOP_IDLE_WAIT_MS;
  }
  void onShutdown() override {
    // Log lines the write gate put off: written now, a short wait for a
    // pause at most (they are a log: within the hook's budget).
    if (svc_) svc_->flushLog(300);
  }
 private:
  static constexpr uint32_t kPurgeMs = 30000;
  std::unique_ptr<PresenceService> svc_;
  uint32_t lastPurgeMs_ = 0;
};

#endif
