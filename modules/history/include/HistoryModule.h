#pragma once
#ifndef ESPRACK_HISTORY_MODULE_H
#define ESPRACK_HISTORY_MODULE_H

// HistoryModule — time series kept on flash without wearing it out
// (docs/plans/history.md). Other modules register series through
// app->history() (history/IHistory.h) and add samples or records; the module
// buffers them in RAM, writes them to /hist in batches, rolls them up, keeps
// them for each tier's retention and serves them over REST for charts and
// tables. The page "History" shows the storage, the writes and the series.
//
//   .install<HistoryModule>()   // after filesystem; before the modules that record
//
// Priority 21: after FileSystemModule (20), before the modules that register
// series in their onInstall/onBegin.

#include <App.h>
#include <Module.h>

#include <memory>

#include "HistoryService.h"

class HistoryModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "history";
    d.version = HISTORY_MODULE_VERSION;
    d.priority = 21;
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new HistoryService(ctx.app, ctx.server, ctx.security, ctx.cfgMgr));
    svc_->registerManifest(ctx.web);
    // Late-bind before any later module's onInstall/onBegin asks for it.
    if (ctx.app) ctx.app->setHistory(svc_.get());
  }
  void onBegin() override {
    if (svc_) svc_->begin();
  }
  void onShutdown() override {
    if (svc_) svc_->end();  // what is waiting goes to flash (if power allows)
  }

 private:
  std::unique_ptr<HistoryService> svc_;
};

#endif  // ESPRACK_HISTORY_MODULE_H
