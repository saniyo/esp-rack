#pragma once
#ifndef ESPRACK_WIREGUARD_MODULE_H
#define ESPRACK_WIREGUARD_MODULE_H

// WireGuardModule — ESPRack Module wrapper for WireguardService.
//
// Priority 14 puts it just after cert-manager (12), so by the time
// other modules install at 30+ they can pull
// `app->wireguard()->publicKey()` for their own logging / UI
// without worrying about ordering. Requires cert-manager because
// the WG private key is encrypted by the same SecretsVault that
// cert-manager unlocks at boot.

#include <Module.h>
#include <App.h>
#include "WireguardService.h"
#include <memory>

// Module version. Bumped to 0.2.0 when the underlying tunnel library
// was switched from ciniml/francescolavra/WireGuard-ESP32-Arduino to
// the static-allocation WireGuard-ESPRack fork (handshake + transport
// pbufs from PBUF_POOL, wireguard_device in BSS — see
// https://github.com/saniyo/WireGuard-ESPRack).
// The library-backend identifier itself lives in WireguardService.h
// as WIREGUARD_BACKEND_NAME so it can be surfaced through the
// /uiManifest WG form without WireguardModule.h being on the include
// path of WireguardService.cpp.
#define WIREGUARD_MODULE_VERSION  "0.2.0"

class WireGuardModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "wireguard";
    d.version = WIREGUARD_MODULE_VERSION;
    d.priority = 14;
    d.requires_ = {"wifi", "cert-manager"};
  }

  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new WireguardService(ctx.cfgMgr));
    // Late-bind into App so MothershipService (priority 35) sees a
    // ready IWireguardProvider in its onInstall.
    if (ctx.app) ctx.app->setWireguard(svc_.get());
    svc_->registerManifest(ctx.web);
  }

  void onBegin() override { if (svc_) svc_->begin(); }
  void onLoop()  override { if (svc_) svc_->loop();  }

  WireguardService* service() { return svc_.get(); }

 private:
  std::unique_ptr<WireguardService> svc_;
};

#endif  // ESPRACK_WIREGUARD_MODULE_H
