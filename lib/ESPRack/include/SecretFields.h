#pragma once
#ifndef ESPRACK_SECRET_FIELDS_H
#define ESPRACK_SECRET_FIELDS_H

// SecretFields — one definition of "a secret field" for the whole web layer.
//
// A form field is secret when its option string starts with "secret;"
// (web::Secret / FormBuilder::addSecretField). The same key names are the
// top-level config keys ConfigManager encrypts at rest and, by convention,
// the keys a feature's flat state reader emits.
//
// Masking policy (LIB-FIX-004):
//   * GET /form and GET /state for non-admin identities: secret values are
//     replaced by kSecretMask (value set) or "" (nothing stored).
//   * WS pushes and POST echoes: always masked — one payload serves every
//     subscriber, and nobody needs a secret live.
//   * Writes (POST /state, WS "p"): a secret key whose value is exactly
//     kSecretMask is dropped before the updater runs, so a form that
//     round-trips the mask never overwrites the stored secret. (Corollary:
//     the literal mask string can't be stored as a secret value.)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>
#include <vector>

namespace SecretFields {

static constexpr const char* kSecretMask = "********";

// Form tree walk: every field object whose "o" starts with "secret;" —
// its single non-"o" key is a secret key. Recurses through tabs/arrays.
inline void collectFromForm(JsonObjectConst root, std::vector<String>& out) {
  const char* o = root["o"] | (const char*)nullptr;
  if (o && strncmp(o, "secret;", 7) == 0) {
    for (JsonPairConst kv : root) {
      if (strcmp(kv.key().c_str(), "o") == 0) continue;
      bool dup = false;
      for (const String& k : out) { if (k == kv.key().c_str()) { dup = true; break; } }
      if (!dup) out.emplace_back(kv.key().c_str());
      break;  // exactly one value key per field
    }
    return;
  }
  for (JsonPairConst kv : root) {
    JsonVariantConst v = kv.value();
    if (v.is<JsonObjectConst>()) {
      collectFromForm(v.as<JsonObjectConst>(), out);
    } else if (v.is<JsonArrayConst>()) {
      for (JsonVariantConst el : v.as<JsonArrayConst>()) {
        if (el.is<JsonObjectConst>()) collectFromForm(el.as<JsonObjectConst>(), out);
      }
    }
  }
}

inline bool hasValue(JsonVariantConst v) {
  if (v.isNull()) return false;
  if (v.is<const char*>()) { const char* s = v.as<const char*>(); return s && s[0]; }
  return true;
}

// Mask secret leaves inside a form tree (field objects marked "secret;").
inline void maskForm(JsonObject root) {
  const char* o = root["o"] | (const char*)nullptr;
  if (o && strncmp(o, "secret;", 7) == 0) {
    for (JsonPair kv : root) {
      if (strcmp(kv.key().c_str(), "o") == 0) continue;
      kv.value().set(hasValue(kv.value()) ? kSecretMask : "");
      break;
    }
    return;
  }
  for (JsonPair kv : root) {
    JsonVariant v = kv.value();
    if (v.is<JsonObject>()) {
      maskForm(v.as<JsonObject>());
    } else if (v.is<JsonArray>()) {
      for (JsonVariant el : v.as<JsonArray>()) {
        if (el.is<JsonObject>()) maskForm(el.as<JsonObject>());
      }
    }
  }
}

// Mask top-level secret keys of a flat state object.
inline void maskFlat(JsonObject root, const std::vector<String>& keys) {
  for (const String& k : keys) {
    if (!root.containsKey(k)) continue;
    JsonVariant v = root[k];
    v.set(hasValue(v) ? kSecretMask : "");
  }
}

// Both shapes: a reader may emit a form tree (single-reader features) or
// flat values; each pass is a no-op on the other shape.
inline void maskAll(JsonObject root, const std::vector<String>& keys) {
  maskForm(root);
  maskFlat(root, keys);
}

// Drop secret keys carrying the mask, at the top level and inside the
// {settings:{...}} envelope the updaters also accept.
inline void stripMasked(JsonObject in, const std::vector<String>& keys) {
  auto stripIn = [&keys](JsonObject obj) {
    for (const String& k : keys) {
      if (!obj.containsKey(k)) continue;
      const char* s = obj[k] | (const char*)nullptr;
      if (s && strcmp(s, kSecretMask) == 0) obj.remove(k);
    }
  };
  stripIn(in);
  if (in.containsKey("settings") && in["settings"].is<JsonObject>()) {
    stripIn(in["settings"].as<JsonObject>());
  }
}

}  // namespace SecretFields

#endif  // ESPRACK_SECRET_FIELDS_H
