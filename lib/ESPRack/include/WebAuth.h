#ifndef WebAuth_h
#define WebAuth_h

#include <SecurityManager.h>

enum class WebAuthLevel : uint8_t { Public, Authenticated, Admin };

inline const char* webAuthLevelToStr(WebAuthLevel lvl) {
  switch (lvl) {
    case WebAuthLevel::Public: return "public";
    case WebAuthLevel::Authenticated: return "authenticated";
    case WebAuthLevel::Admin: return "admin";
  }
  return "public";
}

inline AuthenticationPredicate webAuthLevelToPredicate(WebAuthLevel lvl) {
  switch (lvl) {
    case WebAuthLevel::Public: return AuthenticationPredicates::NONE_REQUIRED;
    case WebAuthLevel::Authenticated: return AuthenticationPredicates::IS_AUTHENTICATED;
    case WebAuthLevel::Admin: return AuthenticationPredicates::IS_ADMIN;
  }
  return AuthenticationPredicates::NONE_REQUIRED;
}

#endif
