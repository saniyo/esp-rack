#include <SecuritySettingsService.h>

#include <PresenceService.h>
#include <ProblemJson.h>
#include <web/Form.h>
#include <WebManager.h>
#include <time.h>

#if FT_ENABLED(FT_SECURITY)

void SecuritySettings::buildJwtForm(SecuritySettings& settings, JsonObject& root) {
  // Single-tab, single-field form — JWT signing key only. The Users
  // table lives on the standalone /security React page; this tab is
  // System-area territory and therefore intentionally minimal.
  web::Form form(root, "jwt", "JWT Secret");
  form.add(web::Message("jwt_info",
      "Changing this secret immediately invalidates every signed JWT — "
      "all logged-in admins/guests will be forced to sign in again.")
      .level("info").icon("Info"));
  form.add(web::Secret("jwt_secret", settings.jwtSecret.c_str()).icon("Lock"));
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
    } else if (s.isEmpty() && settings.jwtSecret.isEmpty()) {
      // Key present but empty AND no secret in memory — the load path
      // cleared an undecryptable ENC: record (foreign device key or
      // vault compiled out). Never sign JWTs with "" — seed a fresh
      // random secret; sessions re-authenticate once.
      settings.jwtSecret = SettingValue::format(FACTORY_JWT_SECRET);
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

  // Full-state feature — registry auto-mounts /api/v1/security/state
  // (GET/POST) and /api/v1/security/form (GET). The standalone /security
  // React page (Users CRUD) uses the state endpoint directly.
  // `routeTemplate = ""` suppresses route emission in WebFeatureEntry::
  // toJson — React's manifestRoutes filter drops the entry because
  // `!entry.route` is true for the empty string. Auth is Authenticated
  // rather than Admin because the React page does an admin-level check
  // itself before showing edit affordances.
  {
    WebFeatureSpec spec;
    spec.id            = "security";
    spec.title         = "Security";
    spec.component     = "";  // bespoke React page, not DynamicFeature
    spec.routeTemplate = "";  // suppress /security/* manifest route
    spec.auth          = WebAuthLevel::Authenticated;
    // restRead/restUpdate not assigned — registry ignores them and
    // derives /api/v1/security/form and /api/v1/security/state itself.
    // Stays EXPLICIT 2048 (not WEBFEATURE_BUFFER_AUTO): the boot probe
    // would only measure the users[] present in the config file, but the
    // operator ADDS users at runtime (~160 B of JSON each) and AUTO+512
    // would silently drop the new rows from the response. 2048 covers
    // the measured 343 B base + ~10 users.
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
  // Registry auto-mounts /api/v1/securityJwt/form (GET) for this feature.
  {
    WebFeatureSpec spec;
    spec.id            = "securityJwt";
    spec.title         = "JWT Secret";
    spec.component     = "";  // sub-tab of `system`
    spec.routeTemplate = "";  // no top-level route, system owns the page
    spec.auth          = WebAuthLevel::Admin;
    // restRead/restUpdate not assigned — registry derives paths itself.
    // Auto-sized: single Secret field + one static message, no runtime
    // growth — the probe lands well under the old hand-guessed 1024.
    _jwtFeature = web->registerFeature<SecuritySettings>(
        std::move(spec), this,
        SecuritySettings::buildJwtForm, SecuritySettings::update,
        WEBFEATURE_BUFFER_AUTO);
  }

  // Wire the JWT form as a tab under the compound `system` feature.
  // restPath is the registry-derived form path for this bespoke page
  // contributed to another feature (plan §3.1: own-feature tabs omit
  // restPath; tabs contributed to OTHER features set it explicitly).
  WebTabSpec tab;
  tab.key      = "jwt";
  tab.title    = "JWT Secret";
  tab.restPath = "/api/v1/securityJwt/form";
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
    log_d("[security] migrating plaintext credentials to PBKDF2 hash");
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

// Wall clock is "synced" once NTP pushed us past this (2023-11-14) —
// before that the RTC still reads epoch-1970 and exp math is meaningless.
static constexpr time_t kClockSaneAfter = 1700000000;

// Human-session token lifetime (api-standardization C0). Machine tokens
// (long-lived, role-scoped) arrive in phase C3 with their own issuance
// path; this constant governs only /signIn-minted tokens.
static constexpr uint32_t kHumanJwtTtlSec = 24UL * 3600UL;

inline void populateJWTPayload(JsonObject& payload, User* user) {
  payload["username"] = user->username;
  // `admin` kept alongside `role` — the React app's session decode still
  // branches on the boolean; role is the forward-facing claim the API
  // role gates (reader/operator/admin, phase C3) will consume.
  payload["admin"] = user->admin;
  payload["role"] = user->admin ? "admin" : "user";
  const time_t now = time(nullptr);
  if (now > kClockSaneAfter) {
    payload["iat"] = (uint32_t)now;
    payload["exp"] = (uint32_t)now + kHumanJwtTtlSec;
  }
  // No sane clock at mint time → no exp claim: the token validates like a
  // legacy one. Better a non-expiring LAN session than a device that
  // can't log anyone in before NTP syncs.
}

boolean SecuritySettingsService::validatePayload(JsonObject& parsedPayload, User* user) {
  // Semantic validation, NOT byte-equality: tokens now carry per-mint
  // claims (iat/exp), so reconstruct-and-compare can't work anymore.
  // Identity and privilege must match the LIVE user record — flipping a
  // user's admin bit still invalidates their old tokens, same as before.
  const char* uname = parsedPayload["username"] | (const char*)nullptr;
  if (!uname || user->username != uname) {
    return false;
  }
  const bool tokenAdmin = parsedPayload["admin"] | false;
  if (tokenAdmin != user->admin) {
    return false;
  }
  // Expiry: enforced only when the token carries exp AND the wall clock
  // is sane. Legacy tokens (no exp) stay valid — no forced re-login on
  // this firmware upgrade; they age out as users re-authenticate.
  const uint32_t exp = parsedPayload["exp"] | 0UL;
  if (exp) {
    const time_t now = time(nullptr);
    if (now > kClockSaneAfter && (uint32_t)now > exp) {
      return false;
    }
  }
  return true;
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

// 401 vs 403 (api-standardization C0): no/invalid token is UNAUTHORIZED;
// a valid identity that fails the predicate is FORBIDDEN. The old
// always-401 made the React app bounce a logged-in guest to the login
// page for endpoints they simply lacked the role for. Bodies are RFC 9457
// problem+json so machine clients branch on `title`, not on prose.
static void sendAuthProblem(AsyncWebServerRequest* request, Authentication& authentication) {
  if (!authentication.authenticated) {
    sendProblem(request, 401, "unauthorized", "Missing or invalid token");
  } else {
    sendProblem(request, 403, "forbidden", "Insufficient role for this endpoint");
  }
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
      sendAuthProblem(request, authentication);
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
      sendAuthProblem(request, authentication);
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
