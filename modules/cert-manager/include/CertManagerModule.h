#pragma once
#ifndef ESPRACK_CERT_MANAGER_MODULE_H
#define ESPRACK_CERT_MANAGER_MODULE_H

// CertManagerModule — wraps CertManagerService into the ESPRack
// Module lifecycle. Priority 12 puts it just after wifi (10) but
// well before any consumer of mTLS (mqtt/auto-update/mothership at
// 35-40), so by the time those install they can call
// `app->cert()->hasValidCert()` and `app->tls()->attachToClient()`
// safely.

#include <Module.h>
#include <App.h>
#include "CertManagerService.h"
#include <memory>

class CertManagerModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "cert-manager"; d.version = "0.1.0"; d.priority = 12;
    d.requires_ = {"wifi"};
  }

  void onInstall(ESPRack::ModuleContext& ctx) override {
    // Pull TLS provider from App — populated by core App ctor in
    // Phase 1.1. Until then it's nullptr; service handles it
    // gracefully (Phase 1.0 stubs are no-op anyway).
    ITLSProvider* tls = ctx.app ? ctx.app->tls() : nullptr;
    svc_.reset(new CertManagerService(ctx.cfgMgr, tls));

    // Late-bind ICertProvider into App so consumers of mTLS can
    // pull the cert state via `app->cert()->hasValidCert()`. Same
    // pattern MqttModule / TelegramModule use.
    if (ctx.app) ctx.app->setCert(svc_.get());

    // Stash app pointer so onBegin can pick up WireGuardModule's
    // late-bound provider once all installs are complete.
    app_ = ctx.app;

    svc_->registerManifest(ctx.web);
  }

  void onBegin() override {
    if (!svc_) return;
    // Phase WG.3 — by the time onBegin runs, every Module's
    // onInstall has already executed (Builder phases the loop:
    // all installs first, then all begins). So app->wireguard()
    // is either populated (WireGuardModule was installed) or
    // permanently null. Bind it once into the cert service so the
    // enroll flow knows whether to include a wg_pubkey field.
    if (app_) svc_->setWireguardProvider(app_->wireguard());
    svc_->begin();
  }
  void onLoop()  override { if (svc_) svc_->loop();  }

  CertManagerService* service() { return svc_.get(); }

 private:
  std::unique_ptr<CertManagerService> svc_;
  ESPRack::App* app_{nullptr};
};

#endif
