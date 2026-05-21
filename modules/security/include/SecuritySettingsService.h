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
#define SECURITY_SETTINGS_PATH       "/rest/securitySettings"
#define SECURITY_JWT_FORM_PATH       "/rest/system/jwt/form"

#if FT_ENABLED(FT_SECURITY)

class SecuritySettings {
 public:
  String jwtSecret;
  std::list<User> users;

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

 private:
  ConfigDelegate<SecuritySettings>     _cfg;
  // Two features — same StatefulService, different paths/auth/readers.
  // /rest/securitySettings (full state, used by /security React Users
  // page) and /rest/system/jwt/form (JWT-only DynamicFeature form
  // under the System tab). WebManager owns both endpoint bindings,
  // dispatches proxy reach, and applies the right auth predicate.
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
