#pragma once
#ifndef ESPRACK_SECURITY_MODULE_H
#define ESPRACK_SECURITY_MODULE_H

// SecurityModule — provides the real PBKDF2-backed SecurityManager
// implementation, plus the /rest/signIn AuthenticationService for the
// React login flow. Replaces the framework's default
// NullSecurityManager via App::setSecurityManager + WebManager::
// setSecurityManager (both swapped early so subsequent modules that
// register auth-protected endpoints capture the real pointer).
//
// Lifecycle ordering: priority 8. Higher than PresenceModule (7) so
// ctx.presence is non-null at install time — the SecuritySettings
// Service uses it to populate client-presence X-headers on every
// wrapped request. Lower than other modules (priority >=10) that
// rely on auth.
//
// FT_SECURITY=0 build: Authentication and SecuritySettingsService
// have stub implementations in their .cpp; SecurityModule still
// installs but contributes nothing functional. Operator-side this
// looks like NullSecurityManager (every request is admin) — same
// behaviour as before this module landed.

#include <Module.h>
#include <App.h>
#include <WebManager.h>
#include "SecuritySettingsService.h"
#include "AuthenticationService.h"
#include <memory>

class SecurityModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id       = "security";
    d.version  = "1.0.0";
    d.priority = 8;
    d.requires_ = {"presence"};   // setPresenceService needs the real ptr
  }

  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new SecuritySettingsService(ctx.server, ctx.cfgMgr));

    // Late-bind the real SecurityManager into App AND WebManager so
    // every subsequent module's wrapRequest/wrapCallback captures it
    // (Builder topo-sort installs us at priority 8 → before the 80+
    // priority modules that surface auth-protected REST endpoints).
    if (ctx.app) ctx.app->setSecurityManager(svc_.get());
    if (ctx.web) ctx.web->setSecurityManager(svc_.get());

    if (ctx.presence) svc_->setPresenceService(ctx.presence);
    svc_->registerManifest(ctx.web);

#if FT_ENABLED(FT_SECURITY)
    // Pass WebManager so the service also registers signIn as a
    // proxy endpoint — Phase 7c mship-ui reverse proxy can then
    // authenticate the operator through the bulk-upload-fronted UI
    // without /rest/signIn 404'ing from rest.proxy.
    auth_.reset(new AuthenticationService(ctx.server, svc_.get(), ctx.web));
#endif
  }

  void onBegin() override { if (svc_) svc_->begin(); }

  // Expose the SecuritySettingsService so application modules can read
  // the user list / JWT secret directly (rare, but useful for code
  // that wants to mint custom tokens).
  SecuritySettingsService* service() { return svc_.get(); }

 private:
  std::unique_ptr<SecuritySettingsService> svc_;
#if FT_ENABLED(FT_SECURITY)
  std::unique_ptr<AuthenticationService>   auth_;
#endif
};

#endif  // ESPRACK_SECURITY_MODULE_H
