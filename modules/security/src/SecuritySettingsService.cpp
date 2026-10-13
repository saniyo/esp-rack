#include <SecuritySettingsService.h>

#include <PresenceService.h>
#include <ProblemJson.h>
#include <web/Form.h>
#include <WebManager.h>
#include <esp_random.h>  // esp_fill_random for machine-token ids
#include <time.h>
#include <time/TimeSync.h>

#include <atomic>

#if FT_ENABLED(FT_SECURITY)

// Token iat/exp need a trustworthy wall clock: ESPRack::TimeSync::clockSane()
// (a time source made the clock valid). Before that exp math is meaningless.

// SES-LIB-022: any user still on its factory password. Written by the updater
// (synchronously, with the password change), read lock-free from any task.
// true until the config is loaded: fail-safe for an early reader.
static std::atomic<bool> s_factoryCreds{true};
// Set when a legacy file (no per-user "factory" flag) had to be checked by
// hashing; begin() then saves so the next boot doesn't hash again.
static bool s_factoryVerified = false;

static bool isFactoryPlain(const String& username, const String& pw) {
  return (username == FACTORY_ADMIN_USERNAME && pw == FACTORY_ADMIN_PASSWORD) ||
         (username == FACTORY_GUEST_USERNAME && pw == FACTORY_GUEST_PASSWORD);
}

// Legacy files only: PBKDF2 of the factory password with the stored salt.
static bool isFactoryHash(const String& username, const String& hash, const String& salt) {
  const char* pw = username == FACTORY_ADMIN_USERNAME   ? FACTORY_ADMIN_PASSWORD
                   : username == FACTORY_GUEST_USERNAME ? FACTORY_GUEST_PASSWORD
                                                        : nullptr;
  if (!pw || hash.isEmpty()) return false;
  s_factoryVerified = true;
  return SecurityHash::hashPassword(pw, salt) == hash;
}

// Human-session token lifetime (api-standardization C0). Machine tokens
// (C3) are minted with their own ~10-year exp in mintMachineToken; this
// constant governs only /api/v1/auth/token-minted human sessions.
static constexpr uint32_t kHumanJwtTtlSec = 24UL * 3600UL;

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
      bool factory;
    };
    std::map<String, PriorHash> prior;
    for (User& u : settings.users) {
      prior[u.username] = {u.password, u.salt, settings.factoryFor(u.username)};
    }

    settings.factoryAdmin = settings.factoryGuest = false;  // re-decided per row below
    settings.users.clear();
    for (JsonVariant user : src["users"].as<JsonArray>()) {
      String username  = user["username"].as<String>();
      String pwField   = user["pwd"].as<String>();
      String saltField = user["salt"].as<String>();
      bool   admin     = user["admin"] | false;

      String finalHash;
      String finalSalt;
      bool isFactory = false;

      auto it = prior.find(username);

      if (pwField.isEmpty() && it != prior.end()) {
        // Edit with blank password → preserve existing credentials.
        finalHash = it->second.hash;
        finalSalt = it->second.salt;
        isFactory = it->second.factory;
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
        if (it != prior.end()) {
          isFactory = it->second.factory;           // unchanged round-trip
        } else if (user["factory"].is<bool>()) {
          isFactory = user["factory"].as<bool>();   // our own file
        } else {
          isFactory = isFactoryHash(username, finalHash, finalSalt);  // legacy file: once
        }
      } else {
        // Treat as plaintext (covers legacy plaintext file load AND a
        // new password typed via the React UserForm modal).
        if (!pwField.isEmpty()) {
          isFactory = isFactoryPlain(username, pwField);  // before hashing
          finalSalt = SecurityHash::makeSalt();
          finalHash = SecurityHash::hashPassword(pwField, finalSalt);
        }
      }

      settings.setFactory(username, isFactory);
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
    settings.factoryAdmin = settings.factoryGuest = true;
    changed = true;
  }

  // The cached flag follows the users synchronously: the reply to a password
  // change already reads the new value.
  s_factoryCreds.store(settings.factoryAdmin || settings.factoryGuest);

  // Machine-token registry round-trip (C3). Present only on the config
  // cold-load path — the React pages never POST api_tokens (mint/revoke
  // go through dedicated admin actions), so absence leaves the in-memory
  // registry alone, mirroring the users[] contract above.
  if (src.containsKey("api_tokens") && src["api_tokens"].is<JsonArray>()) {
    settings.apiTokens.clear();
    for (JsonVariant tv : src["api_tokens"].as<JsonArray>()) {
      if (settings.apiTokens.size() >= SecuritySettings::kMaxApiTokens) break;
      SecuritySettings::ApiToken t;
      t.tid     = tv["tid"]  | "";
      t.name    = tv["name"] | "";
      t.role    = tv["role"] | "reader";
      t.revoked = tv["revoked"] | false;
      if (t.tid.length()) settings.apiTokens.push_back(std::move(t));
    }
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
  // `!entry.route` is true for the empty string. Admin-enforced
  // SERVER-SIDE (C3): the old Authenticated level relied on the React
  // page hiding edit affordances, which left the user table + JWT
  // secret readable/writable by any guest token over raw REST.
  {
    WebFeatureSpec spec;
    spec.id            = "security";
    spec.title         = "Security";
    spec.component     = "";  // bespoke React page, not DynamicFeature
    spec.routeTemplate = "";  // suppress /security/* manifest route
    spec.auth          = WebAuthLevel::Admin;
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

  // Wire the JWT form as a tab under the compound `system` feature —
  // referenced by typed-feature id; the manifest emitter resolves the
  // derived /api/v1/securityJwt/form path.
  WebTabSpec tab;
  tab.key      = "jwt";
  tab.title    = "JWT Secret";
  tab.formOf   = "securityJwt";
  tab.postable = true;
  tab.auth     = WebAuthLevel::Admin;
  tab.order    = 50;
  web->addTabToFeature("system", tab);

  // ---- Machine access tokens (C3) ----------------------------------
  // tokens.create — mints a role-scoped long-lived JWT; the VALUE rides
  // back in the action's message and is shown exactly once in the
  // snackbar. Args arrive as query params via the form's withFields.
  {
    WebActionSpec act;
    act.id             = "tokens.create";
    act.title          = "Create token";
    act.icon           = "Key";
    act.color          = "primary";
    act.auth           = WebAuthLevel::Admin;
    act.handler = [this](AsyncWebServerRequest* r) {
      String name = r->hasArg("name") ? r->arg("name") : String("");
      name.trim();
      if (name.isEmpty()) name = "token";
      // Dropdown delivers the option ordinal — map to the role slug.
      int roleIdx = r->hasArg("role") ? r->arg("role").toInt() : 0;
      const char* role = (roleIdx == 2) ? "admin"
                        : (roleIdx == 1) ? "operator" : "reader";
      String tok = this->mintMachineToken(name, role);
      if (tok.isEmpty()) {
        sendProblem(r, 409, "registry_full",
                    "Token registry is full - revoke and reuse a slot");
        return;
      }
      // Snackbar carries the one-time token value. Built with ArduinoJson
      // (LIB-FIX-008): the operator-supplied name is escaped, so a quote or
      // backslash in it can't break the JSON and lose the shown-once token.
      String message = String(role) + " token '" + name + "' (copy now, shown once): " + tok;
      DynamicJsonDocument doc(JSON_OBJECT_SIZE(2) + message.length() + 64);
      doc["ok"] = true;
      doc["message"] = message;
      String body;
      serializeJson(doc, body);
      r->send(200, "application/json", body);
    };
    web->registerAction(act);
  }
  {
    WebActionSpec act;
    act.id             = "tokens.revoke";
    act.title          = "Revoke token";
    act.icon           = "Block";
    act.color          = "warning";
    act.auth           = WebAuthLevel::Admin;
    act.successMessage = "Token revoked";
    act.handler = [this](AsyncWebServerRequest* r) {
      String tid = r->hasArg("tid") ? r->arg("tid") : String("");
      tid.trim();
      if (tid.isEmpty() || !this->revokeMachineToken(tid)) {
        sendProblem(r, 404, "token_not_found",
                    "No registry entry with that id");
        return;
      }
      r->send(200, "application/json", "{\"ok\":true}");
    };
    web->registerAction(act);
  }

  // Tokens management form — third typed feature on the same service.
  // Auto-sized buffer is safe even though tokens are added at runtime:
  // the adaptive response path grows the doc on overflow.
  {
    WebFeatureSpec spec;
    spec.id            = "apiTokens";
    spec.title         = "API Tokens";
    spec.component     = "";  // sub-tab of `system`
    spec.routeTemplate = "";
    spec.auth          = WebAuthLevel::Admin;
    web->registerFeature<SecuritySettings>(
        std::move(spec), this,
        SecuritySettings::buildTokensForm, SecuritySettings::update,
        WEBFEATURE_BUFFER_AUTO);
  }
  WebTabSpec tokTab;
  tokTab.key      = "tokens";
  tokTab.title    = "API Tokens";
  tokTab.formOf   = "apiTokens";
  tokTab.postable = false;
  tokTab.auth     = WebAuthLevel::Admin;
  tokTab.order    = 55;
  web->addTabToFeature("system", tokTab);
}

void SecuritySettings::buildTokensForm(SecuritySettings& s, JsonObject& root) {
  web::Form form(root, "tokens", "API Tokens");

  form.add(web::Text("tokens_info",
                     "Machine tokens are long-lived role-scoped credentials "
                     "for external servers. The value is shown ONCE at "
                     "creation - copy it from the snackbar.")
               .readOnly().icon("Info"));

  // Registry table. Clicking a row fills the revoke field below.
  form.add(web::Table("tokens_table")
               .col("tid",     "ID",      "text")
               .col("name",    "Name",    "text")
               .col("role",    "Role",    "text")
               .col("revoked", "Revoked", "text")
               .icon("Key")
               .rowFill("revoke_tid=tid"));
  JsonArray rows = form.rowsOf("tokens_table");
  for (const SecuritySettings::ApiToken& t : s.apiTokens) {
    JsonObject row = rows.createNestedObject();
    row["tid"]     = t.tid;
    row["name"]    = t.name;
    row["role"]    = t.role;
    row["revoked"] = t.revoked ? "yes" : "";
  }

  form.add(web::Text("token_name", "").label("New token name").icon("Badge"));
  form.add(web::Dropdown("token_role", 0)
               .option("reader (GET only)",          0)
               .option("operator (read + write)",    1)
               .option("admin (full)",               2)
               .icon("Security"));
  form.add(web::Action("t_create", "Create token")
               .ref("tokens.create")
               .withFields("name=token_name,role=token_role")
               .icon("Key").color("primary").refetchForm());

  form.add(web::Text("revoke_tid", "").label("Token ID to revoke").icon("Block"));
  form.add(web::Action("t_revoke", "Revoke")
               .ref("tokens.revoke")
               .withFields("tid=revoke_tid")
               .icon("Block").color("warning").refetchForm());
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
  // A legacy file had no per-user "factory" flag and was checked by hashing:
  // save it now so the next boot reads the flag instead (SES-LIB-022).
  if (s_factoryVerified) {
    s_factoryVerified = false;
    _cfg.saveIfChanged("factory-flag");
  }
}

bool SecuritySettingsService::usesFactoryCredentials() const { return s_factoryCreds.load(); }

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

    // Machine-token branch (C3): the "tid" claim marks a minted machine
    // token. The signature already proved authenticity; the REGISTRY is
    // the live authority — it supplies the role (so an operator can
    // re-scope without re-minting nothing — role lives in the entry)
    // and the revocation bit. exp enforced same as human tokens.
    const char* tid = parsedPayload["tid"] | (const char*)nullptr;
    if (tid && *tid) {
      const uint32_t exp = parsedPayload["exp"] | 0UL;
      // clockSane() first, then read the clock it vouched for.
      if (exp && ESPRack::TimeSync::clockSane()) {
        const time_t now = time(nullptr);
        if ((uint32_t)now > exp) return Authentication();
      }
      for (const SecuritySettings::ApiToken& t : _state.apiTokens) {
        if (t.tid == tid) {
          if (t.revoked) return Authentication();
          User machine(t.name.length() ? t.name : t.tid, "",
                       t.role == "admin");
          return Authentication(machine, t.role.c_str());
        }
      }
      return Authentication();  // unknown tid — registry wiped or foreign
    }

    String username = parsedPayload["username"];
    for (User _user : _state.users) {
      if (_user.username == username && validatePayload(parsedPayload, &_user)) {
        return Authentication(_user);
      }
    }
  }
  return Authentication();
}

String SecuritySettingsService::mintMachineToken(const String& name, const String& role) {
  if (role != "reader" && role != "operator" && role != "admin") return String();
  if (_state.apiTokens.size() >= SecuritySettings::kMaxApiTokens) return String();

  uint8_t rnd[4];
  esp_fill_random(rnd, sizeof(rnd));
  char tid[9];
  snprintf(tid, sizeof(tid), "%02x%02x%02x%02x", rnd[0], rnd[1], rnd[2], rnd[3]);

  DynamicJsonDocument doc(MAX_JWT_SIZE);
  JsonObject payload = doc.to<JsonObject>();
  payload["tid"]  = tid;
  payload["role"] = role;
  if (ESPRack::TimeSync::clockSane()) {
    const time_t now = time(nullptr);
    payload["iat"] = (uint32_t)now;
    // ~10 years; the registry's revoked bit is the real kill switch.
    payload["exp"] = (uint32_t)now + 315360000UL;
  }
  String token = _jwtHandler.buildJWT(payload);

  SecuritySettings::ApiToken entry;
  entry.tid  = tid;
  entry.name = name;
  entry.role = role;
  _state.apiTokens.push_back(std::move(entry));
  callUpdateHandlers("tokens.create");  // persists via saveIfChanged
  return token;
}

bool SecuritySettingsService::revokeMachineToken(const String& tid) {
  for (SecuritySettings::ApiToken& t : _state.apiTokens) {
    if (t.tid == tid && !t.revoked) {
      t.revoked = true;
      callUpdateHandlers("tokens.revoke");
      return true;
    }
  }
  return false;
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
  // `admin` kept alongside `role` — the React app's session decode still
  // branches on the boolean; role is the forward-facing claim the API
  // role gates (reader/operator/admin, phase C3) will consume.
  payload["admin"] = user->admin;
  payload["role"] = user->admin ? "admin" : "user";
  if (ESPRack::TimeSync::clockSane()) {
    const time_t now = time(nullptr);
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
  if (exp && ESPRack::TimeSync::clockSane()) {
    const time_t now = time(nullptr);
    if ((uint32_t)now > exp) return false;
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
