#pragma once
#ifndef ESPRACK_NULL_SECURITY_MANAGER_H
#define ESPRACK_NULL_SECURITY_MANAGER_H

// NullSecurityManager — the default SecurityManager wired into App
// when no SecurityModule is installed (i.e. ESPRACK_SECURITY=0 or the
// consumer didn't .install<SecurityModule>()). Treats every request
// as authenticated as ADMIN, matching the FT_SECURITY=0 behaviour of
// the legacy ESPReact framework.
//
// The Security module replaces this with the real PBKDF2-backed
// implementation by calling App::setSecurityManager() during onInstall.

#include <Arduino.h>
#include "SecurityManager.h"
#include <Features.h>

#ifndef FACTORY_ADMIN_USERNAME
#define FACTORY_ADMIN_USERNAME "admin"
#endif
#ifndef FACTORY_ADMIN_PASSWORD
#define FACTORY_ADMIN_PASSWORD "admin"
#endif

namespace ESPRack {

class NullSecurityManager : public SecurityManager {
 public:
  NullSecurityManager() :
      admin_user_(FACTORY_ADMIN_USERNAME, FACTORY_ADMIN_PASSWORD, true) {}

  // SecurityManager interface — all gated by FT_SECURITY in the header
  // that declares them, so guard accordingly.
#if FT_ENABLED(FT_SECURITY)
  Authentication authenticate(const String&, const String&) override {
    return Authentication(admin_user_);
  }
  String generateJWT(User*) override { return String(); }
#endif

  Authentication authenticateRequest(AsyncWebServerRequest*) override {
    return Authentication(admin_user_);
  }

  ArRequestFilterFunction filterRequest(AuthenticationPredicate) override {
    return [](AsyncWebServerRequest*) { return true; };
  }

  ArRequestHandlerFunction wrapRequest(ArRequestHandlerFunction onRequest,
                                       AuthenticationPredicate) override {
    return [onRequest](AsyncWebServerRequest* req) { onRequest(req); };
  }

  ArJsonRequestHandlerFunction wrapCallback(ArJsonRequestHandlerFunction onRequest,
                                            AuthenticationPredicate) override {
    return [onRequest](AsyncWebServerRequest* req, JsonVariant& j) { onRequest(req, j); };
  }

 private:
  User admin_user_;
};

}  // namespace ESPRack

#endif  // ESPRACK_NULL_SECURITY_MANAGER_H
