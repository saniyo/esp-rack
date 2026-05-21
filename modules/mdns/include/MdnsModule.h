#pragma once
#ifndef ESPRACK_MDNS_MODULE_H
#define ESPRACK_MDNS_MODULE_H

// MdnsModule — wraps MdnsService into the ESPRack Module lifecycle.
// Priority 22 puts it after wifi (10) — MDNS needs STA up — but
// before consumers that benefit from name-based reachability
// (mothership/auto-update at 35+). They can call
// `app->mdns()->resolveHost(...)` once they install.

#include <Module.h>
#include <App.h>
#include "MdnsService.h"
#include <memory>

class MdnsModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "mdns"; d.version = "0.1.0"; d.priority = 22;
    d.requires_ = {"wifi"};
  }

  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new MdnsService(ctx.server));
    if (ctx.app) ctx.app->setMdns(svc_.get());
    svc_->registerManifest(ctx.web);
  }

  void onBegin() override { if (svc_) svc_->begin(); }
  void onLoop()  override { if (svc_) svc_->loop();  }

  MdnsService* service() { return svc_.get(); }

 private:
  std::unique_ptr<MdnsService> svc_;
};

#endif  // ESPRACK_MDNS_MODULE_H
