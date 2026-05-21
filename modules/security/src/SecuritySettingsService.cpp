#include <SecuritySettingsService.h>

#include <PresenceService.h>
#include <FormBuilder.h>
#include <WebManager.h>

#if FT_ENABLED(FT_SECURITY)

void SecuritySettings::buildJwtForm(SecuritySettings& settings, JsonObject& root) {
  // Single-tab, single-field form — JWT signing key only. The Users
  // table lives on the standalone /security React page; this tab is
  // System-area territory and therefore intentionally minimal.
  JsonArray jwt = FormBuilder::createForm(root, "jwt", "JWT Secret");
  FormBuilder::addMessageField(jwt, "jwt_info",
      "Changing this secret immediately invalidates every signed JWT — "
      "all logged-in admins/guests will be forced to sign in again.",
      level("info"), icon("Info"));
  FormBuilder::addSecretField(jwt, "jwt_secret", AF::RW, settings.jwtSecret.c_str(),
                              icon("Lock"));
}

StateUpdateResult SecuritySettings::update(JsonObject& root, SecuritySettings& settings) {
  // Unwrap the DynamicFeature envelope `{settings: {...}}` if present.
  // Legacy /rest/securitySettings POST sends a flat root; the JWT form
  // POST wraps its single field. Both come through here.
  JsonObject src = root;
  if (root.containsKey("settings") && root["settings"].is<JsonObject>()) {
    src = root["settings"].as<JsonObject>();
  }

  bool changed = false;

  // jwt_secret — always tracked. Empty incoming value means "field
  // wasn't present in this payload" (the JWT form sends it; the user
  // CRUD page also sends it). Only update on a real change.
  if (src.containsKey("jwt_secret")) {
    String s = src["jwt_secret"].as<String>();
    if (!s.isEmpty() && s != settings.jwtSecret) {
      settings.jwtSecret = s;
      changed = true;
    }
  } else if (settings.jwtSecret.isEmpty()) {
    // Cold boot, no file, no payload → seed a random secret.
    settings.jwtSecret = SettingValue::format(FACTORY_JWT_SECRET);
    changed = true;
  }

  // Users — only present on the full-state path. JWT form payload
  // omits `users` and we leave the in-memory list untouched.
  if (src.containsKey("users") && src["users"].is<JsonArray>()) {
    struct PriorHash {
      String hash;
      String salt;
    };
    std::map<String, PriorHash> prior;
    for (User& u : settings.users) {
      prior[u.username] = {u.password, u.salt};
    }

    settings.users.clear();
    for (JsonVariant user : src["users"].as<JsonArray>()) {
      String username  = user["username"].as<String>();
      String pwField   = user["pwd"].as<String>();
      String saltField = user["salt"].as<String>();
      bool   admin     = user["admin"] | false;

      String finalHash;
      String finalSalt;

      auto it = prior.find(username);

      if (pwField.isEmpty() && it != prior.end()) {
        // Edit with blank password → preserve existing credentials.
        finalHash = it->second.hash;
        finalSalt = it->second.salt;
      } else if (SecurityHash::looksHashed(pwField) &&
                 saltField.length() == SecurityHash::SALT_HEX_LEN &&
                 (it == prior.end() ||
                  (saltField == it->second.salt && pwField == it->second.hash))) {
        // Already-hashed payload. Two valid scenarios:
        //   * cold load from disk (prior empty / username not in prior) →
        //     trust the shape; this is our own saved hash + salt.
        //   * HTTP edit round-trip (prior has the user) → require exact
        //     match of the last-served hash + salt so an admin can't
        //     splice in a known-pre-image hash by hand.
        finalHash = pwField;
        finalSalt = saltField;
      } else {
        // Treat as plaintext (covers legacy plaintext file load AND a
        // new password typed via the React UserForm modal).
        if (!pwField.isEmpty()) {
          finalSalt = SecurityHash::makeSalt();
          finalHash = SecurityHash::hashPassword(pwField, finalSalt);
        }
      }

      settings.users.push_back(User(username, finalHash, finalSalt, admin));
    }
    changed = true;
  } else if (settings.users.empty()) {
    // No file and no payload — seed factory defaults (hashed).
    String adminSalt = SecurityHash::makeSalt();
    String guestSalt = SecurityHash::makeSalt();
    settings.users.push_back(
        User(FACTORY_ADMIN_USERNAME,
             SecurityHash::hashPassword(FACTORY_ADMIN_PASSWORD, adminSalt),
             adminSalt, true));
    settings.users.push_back(
        User(FACTORY_GUEST_USERNAME,
             SecurityHash::hashPassword(FACTORY_GUEST_PASSWORD, guestSalt),
             guestSalt, false));
    changed = true;
  }

  return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
}

SecuritySettingsService::SecuritySettingsService(ConfigManager* cfgMgr) :
    _cfg(cfgMgr,
         "security",
         SECURITY_SETTINGS_FILE,
         2048,
         this,
         SecuritySettings::readConfig,
         SecuritySettings::update,
         false /*autoSave*/,
         nullptr /*validator*/,
         SecuritySettings::buildJwtForm /*formReader*/),
    _jwtHandler(FACTORY_JWT_SECRET) {
  // After any update path: refresh JWT key + persist via ConfigManager.
  addUpdateHandler([this](const String& origin) {
    configureJWTHandler();
    _cfg.saveIfChanged(origin);
  }, false);
}

void SecuritySettingsService::registerManifest(WebManager* web) {
  if (!web) return;

  // Full-state feature — /rest/securitySettings carries the entire
  // {jwt_secret, users[]} shape consumed by the standalone /security
  // React page (Users CRUD). No menu entry, no manifest route: the
  // React app owns the /security route directly via AuthenticatedRouting,
  // and DynamicFeature on an auto-derived /security/* would shadow it.
  // `routeTemplate = ""` explicitly suppresses route emission in
  // WebFeatureEntry::toJson — React's manifestRoutes filter drops the
  // entry because `!entry.route` is true for the empty string.
  // Auth is Authenticated rather than Admin because the React page
  // does an admin-level check itself before showing edit affordances;
  // legacy /security clients (older devices on the same network) rely
  // on this lower bar.
  {
    WebFeatureSpec spec;
    spec.id            = "security";
    spec.title         = "Security";
    spec.component     = "";  // bespoke React page, not DynamicFeature
    spec.routeTemplate = "";  // suppress /security/* manifest route
    spec.auth          = WebAuthLevel::Authenticated;
    spec.restRead      = SECURITY_SETTINGS_PATH;
    spec.restUpdate    = SECURITY_SETTINGS_PATH;
    _fullFeature = web->registerFeature<SecuritySettings>(
        std::move(spec), this,
        SecuritySettings::read, SecuritySettings::update,
        2048);
  }

  // JWT-only feature — single-field form rendered as a sub-tab of the
  // compound `system` feature via the addTabToFeature call below.
  // routeTemplate = "" so it doesn't ALSO emit a top-level /securityJwt
  // route (system feature already owns the tab pane). Admin-only since
  // rotating the JWT secret invalidates every issued token.
  {
    WebFeatureSpec spec;
    spec.id            = "securityJwt";
    spec.title         = "JWT Secret";
    spec.component     = "";  // sub-tab of `system`
    spec.routeTemplate = "";  // no top-level route, system owns the page
    spec.auth          = WebAuthLevel::Admin;
    spec.restRead      = SECURITY_JWT_FORM_PATH;
    spec.restUpdate    = SECURITY_JWT_FORM_PATH;
    _jwtFeature = web->registerFeature<SecuritySettings>(
        std::move(spec), this,
        SecuritySettings::buildJwtForm, SecuritySettings::update,
        1024);
  }

  // Wire the JWT form as a tab under the compound `system` feature.
  WebTabSpec tab;
  tab.key      = "jwt";
  tab.title    = "JWT Secret";
  tab.restPath = SECURITY_JWT_FORM_PATH;
  tab.postable = true;
  tab.auth     = WebAuthLevel::Admin;
  tab.order    = 50;
  web->addTabToFeature("system", tab);
}

void SecuritySettingsService::begin() {
  (void)_cfg.ensureLoaded();
  configureJWTHandler();

  // Legacy plaintext migration — described in detail in commit
  // "feat(security): PBKDF2-SHA256 hashing for admin/guest creds".
  // ConfigDelegate's update path already hashed any plaintext rows
  // during readFromFS, so we just need to flush once if the file shape
  // is stale.
  bool needsMigration = false;
  for (const User& u : _state.users) {
    const bool hashOk = (u.password.length() == SecurityHash::HASH_HEX_LEN) &&
                        SecurityHash::looksHashed(u.password);
    const bool saltOk = (u.salt.length() == SecurityHash::SALT_HEX_LEN);
    if ((!hashOk && !u.password.isEmpty()) || !saltOk) {
      needsMigration = true;
      break;
    }
  }
  if (needsMigration) {
    Serial.println(F("[security] migrating plaintext credentials to PBKDF2 hash"));
    _cfg.saveIfChanged("hash-migration");
  }
}

Authentication SecuritySettingsService::authenticateRequest(AsyncWebServerRequest* request) {
  const AsyncWebHeader* authorizationHeader = request->getHeader(AUTHORIZATION_HEADER);
  if (authorizationHeader) {
    String value = authorizationHeader->value();
    if (value.startsWith(AUTHORIZATION_HEADER_PREFIX)) {
      value = value.substring(AUTHORIZATION_HEADER_PREFIX_LEN);
      return authenticateJWT(value);
    }
  } else if (request->hasParam(ACCESS_TOKEN_PARAMATER)) {
    const AsyncWebParameter* tokenParamater = request->getParam(ACCESS_TOKEN_PARAMATER);
    String value = tokenParamater->value();
    return authenticateJWT(value);
  }
  return Authentication();
}

void SecuritySettingsService::configureJWTHandler() {
  _jwtHandler.setSecret(_state.jwtSecret);
}

Authentication SecuritySettingsService::authenticateJWT(String& jwt) {
  DynamicJsonDocument payloadDocument(MAX_JWT_SIZE);
  _jwtHandler.parseJWT(jwt, payloadDocument);
  if (payloadDocument.is<JsonObject>()) {
    JsonObject parsedPayload = payloadDocument.as<JsonObject>();
    String username = parsedPayload["username"];
    for (User _user : _state.users) {
      if (_user.username == username && validatePayload(parsedPayload, &_user)) {
        return Authentication(_user);
      }
    }
  }
  return Authentication();
}

Authentication SecuritySettingsService::authenticate(const String& username, const String& password) {
  for (User _user : _state.users) {
    if (_user.username != username) continue;
    if (SecurityHash::verify(password, _user.salt, _user.password)) {
      return Authentication(_user);
    }
  }
  return Authentication();
}

inline void populateJWTPayload(JsonObject& payload, User* user) {
  payload["username"] = user->username;
  payload["admin"] = user->admin;
}

boolean SecuritySettingsService::validatePayload(JsonObject& parsedPayload, User* user) {
  DynamicJsonDocument jsonDocument(MAX_JWT_SIZE);
  JsonObject payload = jsonDocument.to<JsonObject>();
  populateJWTPayload(payload, user);
  return payload == parsedPayload;
}

String SecuritySettingsService::generateJWT(User* user) {
  DynamicJsonDocument jsonDocument(MAX_JWT_SIZE);
  JsonObject payload = jsonDocument.to<JsonObject>();
  populateJWTPayload(payload, user);
  return _jwtHandler.buildJWT(payload);
}

ArRequestFilterFunction SecuritySettingsService::filterRequest(AuthenticationPredicate predicate) {
  return [this, predicate](AsyncWebServerRequest* request) {
    Authentication authentication = authenticateRequest(request);
    return predicate(authentication);
  };
}

ArRequestHandlerFunction SecuritySettingsService::wrapRequest(ArRequestHandlerFunction onRequest,
                                                              AuthenticationPredicate predicate) {
  return [this, onRequest, predicate](AsyncWebServerRequest* request) {
    // Touch presence FIRST — even an unauthorised 401 tells us a tab is
    // attempting to talk to us, useful for diagnostics. No-op if no
    // presence service wired.
    if (_presence) _presence->touchFromRequest(request);
    Authentication authentication = authenticateRequest(request);
    if (!predicate(authentication)) {
      request->send(401);
      return;
    }
    onRequest(request);
  };
}

ArJsonRequestHandlerFunction SecuritySettingsService::wrapCallback(ArJsonRequestHandlerFunction onRequest,
                                                                   AuthenticationPredicate predicate) {
  return [this, onRequest, predicate](AsyncWebServerRequest* request, JsonVariant& json) {
    if (_presence) _presence->touchFromRequest(request);
    Authentication authentication = authenticateRequest(request);
    if (!predicate(authentication)) {
      request->send(401);
      return;
    }
    onRequest(request, json);
  };
}

#else

User ADMIN_USER = User(FACTORY_ADMIN_USERNAME, FACTORY_ADMIN_PASSWORD, true);

SecuritySettingsService::SecuritySettingsService(ConfigManager* cfgMgr) : SecurityManager() {
}
SecuritySettingsService::~SecuritySettingsService() {
}

ArRequestFilterFunction SecuritySettingsService::filterRequest(AuthenticationPredicate predicate) {
  return [this, predicate](AsyncWebServerRequest* request) { return true; };
}

// Return the admin user on all request - disabling security features
Authentication SecuritySettingsService::authenticateRequest(AsyncWebServerRequest* request) {
  return Authentication(ADMIN_USER);
}

// Even with FT_SECURITY disabled we still want presence tracking — capture
// X-Client-Key / X-Current-Page before delegating to the real handler.
ArRequestHandlerFunction SecuritySettingsService::wrapRequest(ArRequestHandlerFunction onRequest,
                                                              AuthenticationPredicate predicate) {
  return [this, onRequest](AsyncWebServerRequest* request) {
    if (_presence) _presence->touchFromRequest(request);
    onRequest(request);
  };
}

ArJsonRequestHandlerFunction SecuritySettingsService::wrapCallback(ArJsonRequestHandlerFunction onRequest,
                                                                   AuthenticationPredicate predicate) {
  return [this, onRequest](AsyncWebServerRequest* request, JsonVariant& json) {
    if (_presence) _presence->touchFromRequest(request);
    onRequest(request, json);
  };
}

#endif
