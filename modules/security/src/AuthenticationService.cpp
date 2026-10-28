#include <AuthenticationService.h>
#include <AdaptiveJson.h>
#include <WebManager.h>

#if FT_ENABLED(FT_SECURITY)

AuthenticationService::AuthenticationService(AsyncWebServer* server,
                                              SecurityManager* securityManager,
                                              WebManager* web) :
    _securityManager(securityManager),
    _signInHandler(SIGN_IN_PATH,
                   std::bind(&AuthenticationService::signIn, this, std::placeholders::_1, std::placeholders::_2)) {
  server->on(VERIFY_AUTHORIZATION_PATH,
             HTTP_GET,
             std::bind(&AuthenticationService::verifyAuthorization, this, std::placeholders::_1));
  _signInHandler.setMethod(HTTP_POST);
  _signInHandler.setMaxContentLength(MAX_AUTHENTICATION_SIZE);
  server->addHandler(&_signInHandler);

  // Phase 7c — mship-ui reverse proxy needs to authenticate the
  // operator without traversing AsyncWebServer (proxyDispatch walks
  // WebManager registry, not the server's route table). Register the
  // same signIn flow as a proxy endpoint so the operator's React app
  // can mint a JWT through the reverse-proxy path (/api/v1/auth/token).
  if (web) {
    web->registerProxyEndpoint(
        "POST", SIGN_IN_PATH,
        [this](JsonVariant body, JsonVariant out, int& out_status) {
          out_status = this->authenticateAndMintToken(body, out);
          return true;
        });
    // Phase 7c — /api/v1/auth/verify via proxy. The operator already
    // proved identity at mship login, so we treat the proxy as a
    // trusted caller and always answer 200 (matches the "auth bypass"
    // policy that rest.proxy follows for other endpoints). Without
    // this the React app's JWT-validity check 404'd through the
    // proxy and the app sat on a perpetual "verifying..." spinner.
    web->registerProxyEndpoint(
        "GET", VERIFY_AUTHORIZATION_PATH,
        [](JsonVariant /*body*/, JsonVariant /*out*/, int& out_status) {
          out_status = 200;
          return true;
        });
  }
}

int AuthenticationService::authenticateAndMintToken(JsonVariantConst body,
                                                     JsonVariant out) {
  if (!body.is<JsonObjectConst>()) {
    return 400;
  }
  String username = body["username"] | "";
  String password = body["pwd"]      | "";
  Authentication authentication =
      _securityManager->authenticate(username, password);
  if (!authentication.authenticated) {
    return 401;
  }
  JsonObject obj = out.to<JsonObject>();
  obj["access_token"] = _securityManager->generateJWT(authentication.user);
  return 200;
}

void AuthenticationService::verifyAuthorization(AsyncWebServerRequest* request) {
  Authentication authentication = _securityManager->authenticateRequest(request);
  request->send(authentication.authenticated ? 200 : authentication.busy ? 503 : 401);
}

void AuthenticationService::signIn(AsyncWebServerRequest* request, JsonVariant& json) {
  // Authenticate before entering the adaptive-JSON path so the fill
  // lambda is a pure re-runnable computation (JWT mint from an already-
  // verified user) rather than a side-effectful DB lookup.
  if (!json.is<JsonObjectConst>()) { request->send(400); return; }
  String username = json["username"] | "";
  String password = json["pwd"]      | "";
  Authentication authentication = _securityManager->authenticate(username, password);
  if (!authentication.authenticated) { request->send(401); return; }

  // Token response is tiny: {"access_token":"<jwt>"} ≈ 200 B.
  // Start hint 512 — covers the JWT with a factor-of-2 margin and
  // avoids a grow retry on the common path.
  sendAdaptiveJson(request, [this, &authentication](JsonObject& o) {
    o["access_token"] = _securityManager->generateJWT(authentication.user);
  }, 512);
}

#endif // end FT_ENABLED(FT_SECURITY)
