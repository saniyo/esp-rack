#ifndef SecuritySettingsService_h
#define SecuritySettingsService_h

#include <SettingValue.h>
#include <Features.h>
#include <SecurityManager.h>
#include <SecurityHash.h>
#include <StatefulService.h>
#include <ConfigManager.h>
#include <ConfigDelegate.h>
#include <WebFeatureDelegate.h>
#include <map>

class PresenceService;
class WebManager;

#ifndef FACTORY_JWT_SECRET
#define FACTORY_JWT_SECRET "#{random}-#{random}"
#endif

#ifndef FACTORY_ADMIN_USERNAME
#define FACTORY_ADMIN_USERNAME "admin"
#endif

#ifndef FACTORY_ADMIN_PASSWORD
#define FACTORY_ADMIN_PASSWORD "admin"
#endif

#ifndef FACTORY_GUEST_USERNAME
#define FACTORY_GUEST_USERNAME "guest"
#endif

#ifndef FACTORY_GUEST_PASSWORD
#define FACTORY_GUEST_PASSWORD "guest"
#endif

#define SECURITY_SETTINGS_FILE       "/config/securitySettings.json"
// No REST path constants here: both features ("security", "securityJwt")
// are typed — the registry derives /api/v1/<id>/form|state, and the
// typed-feature proxyDispatch matches the same derived paths, so no
// separate proxy registration (and no path string) is needed.

#if FT_ENABLED(FT_SECURITY)

class SecuritySettings {
 public:
  String jwtSecret;
  std::list<User> users;

  // ---- Machine access tokens (C3, api-v1) -------------------------------
  // The token VALUE is never stored: it is a JWT signed with jwtSecret
  // carrying {tid, role, iat, exp≈10y} and shown exactly once at mint
  // time. The device keeps only this registry — the signature proves
  // authenticity, the registry provides revocation and is the live
  // authority for the role. Bounded (kMaxApiTokens) so the config file
  // and the System tab table stay flat-heap friendly.
  struct ApiToken {
    String tid;       // 8-hex random id (claim "tid" in the JWT)
    String name;      // operator-chosen label ("mothership", "grafana"…)
    String role;      // reader | operator | admin
    bool   revoked{false};
  };
  static constexpr size_t kMaxApiTokens = 8;
  std::list<ApiToken> apiTokens;

  // Does the factory password of the factory admin / guest still work
  //? Only those two usernames can be "factory". Decided when a
  // password is set (before hashing) and persisted per row as "factory", so
  // a boot never re-hashes. See SecuritySettingsService::usesFactoryCredentials.
  bool factoryAdmin = false;
  bool factoryGuest = false;
  bool factoryFor(const String& u) const {
    return u == FACTORY_ADMIN_USERNAME ? factoryAdmin : u == FACTORY_GUEST_USERNAME ? factoryGuest : false;
  }
  void setFactory(const String& u, bool f) {
    if (u == FACTORY_ADMIN_USERNAME) factoryAdmin = f;
    else if (u == FACTORY_GUEST_USERNAME) factoryGuest = f;
  }

  // ---- Reader used by ConfigDelegate (file) AND legacy /rest/securitySettings.
  // The /security React page round-trips this entire shape — `password`
  // round-trips as the PBKDF2 hex hash, `salt` round-trips as the per-user
  // salt; the page never displays them but preserves them on save so that
  // unchanged rows stay stable. Empty `password` on POST = "keep existing
  // hash" (handled by update()).
  static void readConfig(SecuritySettings& settings, JsonObject& root) {
    root["jwt_secret"] = settings.jwtSecret;
    JsonArray users = root.createNestedArray("users");
    for (User user : settings.users) {
      JsonObject userRoot = users.createNestedObject();
      userRoot["username"] = user.username;
      userRoot["pwd"]      = user.password;
      userRoot["salt"]     = user.salt;
      userRoot["admin"]    = user.admin;
      userRoot["factory"]  = settings.factoryFor(user.username);
    }
    JsonArray toks = root.createNestedArray("api_tokens");
    for (const ApiToken& t : settings.apiTokens) {
      JsonObject tr = toks.createNestedObject();
      tr["tid"]     = t.tid;
      tr["name"]    = t.name;
      tr["role"]    = t.role;
      tr["revoked"] = t.revoked;
    }
  }

  // ---- Reader for HTTP wire path (same shape as readConfig — legacy
  // /rest/securitySettings serves the full state).
  static void read(SecuritySettings& settings, JsonObject& root) {
    readConfig(settings, root);
  }

  // ---- One-field form schema for the System → JWT tab.
  // Renders just the jwt_secret input with passwordMask + eye toggle.
  static void buildJwtForm(SecuritySettings& settings, JsonObject& root);

  // ---- Machine-token registry form for the System → API Tokens tab
  // (table of {tid,name,role,revoked} + create/revoke action fields).
  static void buildTokensForm(SecuritySettings& settings, JsonObject& root);

  // ---- Updater shared by every entry point.
  // Two incoming shapes:
  //   1. Full state: { jwt_secret, users: [...] } — used by /security and
  //      by ConfigDelegate cold load.
  //   2. JWT-only form envelope: { settings: { jwt_secret } } — used by
  //      the System/JWT tab. users[] absent → in-memory list preserved.
  // Hash/round-trip detection inside ensures unchanged user rows keep
  // their stored PBKDF2 hash, plaintext rows get hashed once, and a
  // missing users[] leaves the list alone.
  static StateUpdateResult update(JsonObject& root, SecuritySettings& settings);
};

class SecuritySettingsService : public StatefulService<SecuritySettings>, public SecurityManager {
 public:
  SecuritySettingsService(ConfigManager* cfgMgr);

  void registerManifest(WebManager* web);
  void begin();

  // Functions to implement SecurityManager
  Authentication authenticate(const String& username, const String& password);
  bool usesFactoryCredentials() const override;
  Authentication authenticateRequest(AsyncWebServerRequest* request);
  String generateJWT(User* user);
  ArRequestFilterFunction filterRequest(AuthenticationPredicate predicate);
  ArRequestHandlerFunction wrapRequest(ArRequestHandlerFunction onRequest, AuthenticationPredicate predicate);
  ArJsonRequestHandlerFunction wrapCallback(ArJsonRequestHandlerFunction callback, AuthenticationPredicate predicate);

  // Wire a presence service so every wrapped request (REST handler going
  // through wrapRequest / wrapCallback) updates the client registry from
  // X-Client-Key / X-Current-Page headers. Optional — null pointer means
  // no tracking. Call once after PresenceService is constructed.
  void setPresenceService(PresenceService* presence) { _presence = presence; }

  // ---- Machine access tokens (C3) ----------------------------------
  // Mints a long-lived role-scoped JWT and records {tid,name,role} in
  // the registry. Returns the token value (caller shows it ONCE) or an
  // empty String when the registry is full / the role is invalid.
  String mintMachineToken(const String& name, const String& role);
  // Marks the registry entry revoked — the token's signature stays
  // valid but authenticateJWT refuses it from the next request on.
  bool revokeMachineToken(const String& tid);

 private:
  ConfigDelegate<SecuritySettings>     _cfg;
  // Two features — same StatefulService, different auth/readers.
  // "security" feature: /api/v1/security/state (full state, used by
  // /security React Users page). "securityJwt" feature: auto-derived
  // /api/v1/securityJwt/form (JWT-only DynamicFeature form under the
  // System tab). WebManager owns both endpoint bindings, dispatches
  // proxy reach, and applies the right auth predicate.
  WebFeatureEntry<SecuritySettings>*   _fullFeature{nullptr};
  WebFeatureEntry<SecuritySettings>*   _jwtFeature{nullptr};
  ArduinoJsonJWT                       _jwtHandler;
  PresenceService*                     _presence{nullptr};

  void configureJWTHandler();

  /*
   * Lookup the user by JWT
   */
  Authentication authenticateJWT(String& jwt);

  /*
   * Verify the payload is correct
   */
  boolean validatePayload(JsonObject& parsedPayload, User* user);
};

#else

class SecuritySettingsService : public SecurityManager {
 public:
  SecuritySettingsService(ConfigManager* cfgMgr);
  ~SecuritySettingsService();

  // No-op manifest hook keeps ESPReact wiring identical between FT modes.
  void registerManifest(WebManager*) {}
  void begin() {}

  // minimal set of functions to support framework with security settings disabled
  Authentication authenticateRequest(AsyncWebServerRequest* request);
  ArRequestFilterFunction filterRequest(AuthenticationPredicate predicate);
  ArRequestHandlerFunction wrapRequest(ArRequestHandlerFunction onRequest, AuthenticationPredicate predicate);
  ArJsonRequestHandlerFunction wrapCallback(ArJsonRequestHandlerFunction onRequest, AuthenticationPredicate predicate);

  // Same optional presence hook as the FT_SECURITY variant above —
  // touchFromRequest is a no-op when either pointer is null, so this is
  // safe regardless of which build mode is active.
  void setPresenceService(PresenceService* presence) { _presence = presence; }

 private:
  PresenceService* _presence{nullptr};
};

#endif  // end FT_ENABLED(FT_SECURITY)
#endif  // end SecuritySettingsService_h
