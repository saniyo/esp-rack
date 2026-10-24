#ifndef SecurityManager_h
#define SecurityManager_h

#include <Features.h>
#include <ArduinoJsonJWT.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <list>

#define ACCESS_TOKEN_PARAMATER "access_token"

#define AUTHORIZATION_HEADER "Authorization"
#define AUTHORIZATION_HEADER_PREFIX "Bearer "
#define AUTHORIZATION_HEADER_PREFIX_LEN 7

// Sized for the C0 payload: {username, admin, role, iat, exp} parses into
// an ArduinoJson pool comfortably under 256 B (was 128 for the two-claim
// legacy payload).
#define MAX_JWT_SIZE 256

class User {
 public:
  String username;
  // Stored as PBKDF2-SHA256 hex hash on disk (legacy plaintext rows are
  // hashed on first load). Empty password is allowed for default
  // factory rows that have not yet been migrated.
  String password;
  // Per-user random salt (hex of 16 bytes). Empty for legacy rows that
  // still hold plaintext in `password`; the loader detects this and
  // generates a salt + rehashes on first read.
  String salt;
  bool admin;

 public:
  User(String username, String password, bool admin) : username(username), password(password), salt(), admin(admin) {
  }
  User(String username, String password, String salt, bool admin) :
      username(username), password(password), salt(salt), admin(admin) {
  }
};

class Authentication {
 public:
  User* user;
  boolean authenticated;
  // C3 (api-v1): the effective role this identity carries. Humans get
  // "admin"/"user" from their account; machine tokens carry the role
  // chosen at mint time ("reader"/"operator"/"admin"), resolved against
  // the device's token registry (the registry is the revocation/role
  // authority, not the signed claim).
  String role;

 public:
  Authentication(User& user)
      : user(new User(user)),
        authenticated(true),
        role(user.admin ? "admin" : "user") {
  }
  Authentication(User& user, const char* explicitRole)
      : user(new User(user)), authenticated(true), role(explicitRole) {
  }
  Authentication() : user(nullptr), authenticated(false) {
  }
  ~Authentication() {
    delete (user);
  }
};

typedef std::function<boolean(Authentication& authentication)> AuthenticationPredicate;

class AuthenticationPredicates {
 public:
  static bool NONE_REQUIRED(Authentication& authentication) {
    return true;
  };
  static bool IS_AUTHENTICATED(Authentication& authentication) {
    return authentication.authenticated;
  };
  static bool IS_ADMIN(Authentication& authentication) {
    return authentication.authenticated && authentication.user->admin;
  };
  // C3: write-capable identities. A "reader" machine token may GET
  // everything its auth level allows but may not mutate — state POSTs
  // and actions compose this on top of the endpoint's own predicate
  // (failures surface as 403 forbidden, not 401).
  static bool IS_OPERATOR(Authentication& authentication) {
    return authentication.authenticated && authentication.role != "reader";
  };
};

class SecurityManager {
 public:
#if FT_ENABLED(FT_SECURITY)
  /*
   * Authenticate, returning the user if found
   */
  virtual Authentication authenticate(const String& username, const String& password) = 0;

  /*
   * Generate a JWT for the user provided
   */
  virtual String generateJWT(User* user) = 0;

#endif

  /*
   * Check the request header for the Authorization token
   */
  virtual Authentication authenticateRequest(AsyncWebServerRequest* request) = 0;

  /**
   * Filter a request with the provided predicate, only returning true if the predicate matches.
   */
  virtual ArRequestFilterFunction filterRequest(AuthenticationPredicate predicate) = 0;

  /**
   * Wrap the provided request to provide validation against an AuthenticationPredicate.
   */
  virtual ArRequestHandlerFunction wrapRequest(ArRequestHandlerFunction onRequest,
                                               AuthenticationPredicate predicate) = 0;

  /**
   * Wrap the provided json request callback to provide validation against an AuthenticationPredicate.
   */
  // True while a factory login (FACTORY_ADMIN_* / FACTORY_GUEST_* username and
  // password) still works. A cached flag — no hashing, no lock,
  // safe from any task; consumers that drive actuators gate automation on it.
  // false here and in NullSecurityManager (no credentials at all).
  virtual bool usesFactoryCredentials() const { return false; }

  virtual ArJsonRequestHandlerFunction wrapCallback(ArJsonRequestHandlerFunction onRequest,
                                                    AuthenticationPredicate predicate) = 0;
};

#endif  // end SecurityManager_h
