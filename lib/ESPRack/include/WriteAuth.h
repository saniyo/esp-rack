#pragma once
#ifndef ESPRACK_WRITE_AUTH_H
#define ESPRACK_WRITE_AUTH_H

// WriteAuth — server-side write authorization for typed features (LIB-FIX-011).
//
// The UI declares per-tab auth (WebTabSpec::auth, e.g. an admin-only
// Settings tab), but the server used to enforce only the FEATURE level on
// POST /state and WS writes, so any non-reader identity could change
// settings the UI hides from it. WriteAuthMap closes that:
//
//   * at boot the feature's form tree is scanned; every value key is mapped
//     to the write level of the tab(s) whose form contains it: the tab's
//     explicit writeAuth, else the tab's auth (a key in several tabs takes
//     the strictest level);
//   * a write needs max(feature floor, level of every key it touches),
//     where the floor is max(feature auth, feature writeAuth); keys found
//     in no tab need the floor. A {"<tabKey>": {...}} envelope (e.g.
//     {"settings":{...}}) maps its inner keys through that tab.
//   * feature writeAuth (WebFeature::writeAuth / adminWrite) therefore makes
//     EVERY write of the feature need at least that level — the one-liner
//     for "settings of this module are admin-only".
//
// Enforced on POST /api/v1/<id>/state (403 problem+json) and WS "p" frames
// (dropped). rest.proxy (mothership, mTLS-trusted) is not gated.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

#include <WebAuth.h>

struct WebTabSpec;

class WriteAuthMap {
 public:
  // Build from a feature's rendered form tree + its tab specs.
  void build(JsonObjectConst formRoot, const std::vector<WebTabSpec>& tabs,
             WebAuthLevel featureAuth, WebAuthLevel featureWriteAuth);

  // Level a write of `body` requires.
  WebAuthLevel required(JsonObjectConst body) const;

  // True when some write needs more than the feature auth (the request's
  // identity must then be checked against required()).
  bool elevated() const { return _elevated; }

  static bool atLeast(WebAuthLevel have, WebAuthLevel need) {
    return static_cast<uint8_t>(have) >= static_cast<uint8_t>(need);
  }

 private:
  struct KeyLevel {
    String key;
    WebAuthLevel level;
  };
  struct TabLevel {
    String key;
    WebAuthLevel level;
  };
  std::vector<KeyLevel> _keys;
  std::vector<TabLevel> _tabs;
  WebAuthLevel _featureAuth{WebAuthLevel::Authenticated};
  WebAuthLevel _floor{WebAuthLevel::Authenticated};
  bool _elevated{false};

  WebAuthLevel levelOfKey(const char* key) const;
  const TabLevel* tab(const char* key) const;
};

#endif  // ESPRACK_WRITE_AUTH_H
