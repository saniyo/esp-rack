#pragma once
#ifndef ESPRACK_MOTHERSHIP_MODULE_H
#define ESPRACK_MOTHERSHIP_MODULE_H

// MothershipModule — wraps MothershipService into the ESPRack Module
// lifecycle. Priority 35 places it after `cert-manager` (12) and any
// other service the dispatch may delegate to (auto-update at 40 etc.
// — wait, that's higher; we install BEFORE auto-update in topo order
// because mothership doesn't yet depend on its concrete service, only
// on its existence at command-dispatch time which is mid-loop).
//
// requires: cert-manager — without an enrolled cert, mTLS check-in
// can't work; service handles the NeedsCert state gracefully but
// the dependency makes ordering explicit.

#include <Module.h>
#include <App.h>
#include "MothershipService.h"
#include <memory>

class IWireguardProvider;

class MothershipModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "mothership"; d.version = "0.1.0"; d.priority = 35;
    d.requires_ = {"wifi", "cert-manager"};
  }

  void onInstall(ESPRack::ModuleContext& ctx) override {
    ITLSProvider*       tls  = ctx.app ? ctx.app->tls()  : nullptr;
    ICertProvider*      cert = ctx.app ? ctx.app->cert() : nullptr;
    // Optional — only present when WireGuardModule is installed
    // ahead of us (priority 14 < 35). When absent, openTunnel
    // actions gracefully fall through with "no wireguard provider".
    IWireguardProvider* wg   = ctx.app ? ctx.app->wireguard() : nullptr;
    svc_.reset(new MothershipService(ctx.cfgMgr, tls, cert, wg));

    // Late-bind IMothershipProvider into App. Phase 3 (WireGuard
    // module) and any other later consumer can pull state via
    // `app->mothership()->state()` etc.
    if (ctx.app) ctx.app->setMothership(svc_.get());

    // Same service also provides IMothershipProfileProvider —
    // cert-manager reads enroll/recover URLs through this so a
    // profile switch in the Mothership UI applies to PKI flows
    // transparently.
    if (ctx.app) ctx.app->setMothershipProfile(svc_.get());

    svc_->registerManifest(ctx.web);
  }

  void onBegin() override { if (svc_) svc_->begin(); }
  void onLoop()  override { if (svc_) svc_->loop();  }

  MothershipService* service() { return svc_.get(); }

 private:
  std::unique_ptr<MothershipService> svc_;
};

#endif
