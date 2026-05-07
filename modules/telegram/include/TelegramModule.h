#pragma once
#ifndef ESPRACK_TELEGRAM_MODULE_H
#define ESPRACK_TELEGRAM_MODULE_H

#include <Module.h>
#include <App.h>
#include "TelegramService.h"
#include <memory>

class TelegramModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "telegram"; d.version = "2.0.0"; d.priority = 40;
    d.requires_ = {"wifi"};
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new TelegramService(ctx.cfgMgr));
    // Late-bind ITelegramProvider into App so any later-installed
    // service (Builder topo-sorts on priority + requires_) can
    // ctx.app->telegram()->subscribe(...) in its onBegin. Modules
    // installed BEFORE telegram won't see it via ctx — they'd need
    // to query ctx.app at onBegin time when all installs are done.
    if (ctx.app) ctx.app->setTelegram(svc_.get());
    svc_->registerManifest(ctx.web);
  }
  void onBegin() override { if (svc_) svc_->begin(); }

  // Service accessor — useful for App-level integration tests / direct
  // legacy enqueue.
  TelegramService* service() { return svc_.get(); }
 private:
  std::unique_ptr<TelegramService> svc_;
};

#endif
