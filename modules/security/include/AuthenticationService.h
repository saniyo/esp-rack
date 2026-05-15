#ifndef AuthenticationService_H_
#define AuthenticationService_H_

#include <Features.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <SecurityManager.h>

class WebManager;

#define VERIFY_AUTHORIZATION_PATH "/rest/verifyAuthorization"
#define SIGN_IN_PATH "/rest/signIn"

#define MAX_AUTHENTICATION_SIZE 256

#if FT_ENABLED(FT_SECURITY)

class AuthenticationService {
 public:
  // `web` is optional. When present the service ALSO registers the
  // signIn flow as a proxy endpoint so the mothership mship-ui
  // reverse proxy (Phase 7c) can authenticate the operator without
  // going through the device's direct AsyncWebServer route. Same
  // SecurityManager, same JWT minting — just a second mount.
  AuthenticationService(AsyncWebServer* server,
                         SecurityManager* securityManager,
                         WebManager* web = nullptr);

 private:
  SecurityManager* _securityManager;
  AsyncCallbackJsonWebHandler _signInHandler;

  // endpoint functions
  void signIn(AsyncWebServerRequest* request, JsonVariant& json);
  void verifyAuthorization(AsyncWebServerRequest* request);

  // Shared auth path used by both signIn handlers (the AsyncWebServer
  // one and the WebManager proxy one). Writes JWT into `out` on
  // success or leaves it empty on failure; returns the HTTP status
  // to emit.
  int authenticateAndMintToken(JsonVariantConst body, JsonVariant out);
};

#endif  // end FT_ENABLED(FT_SECURITY)
#endif  // end SecurityManager_h
