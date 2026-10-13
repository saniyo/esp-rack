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

// Form tree walk: every field object whose "o" starts with "secret;" -
// its single non-"o" key is a secret key (deduplicated). Recurses through
// tabs/arrays.
void collectFromForm(JsonObjectConst root, std::vector<String>& out);

bool hasValue(JsonVariantConst v);

// Mask secret leaves inside a form tree (field objects marked "secret;").
void maskForm(JsonObject root);

// Mask top-level secret keys of a flat state object.
void maskFlat(JsonObject root, const std::vector<String>& keys);

// Both shapes: a reader may emit a form tree (single-reader features) or
// flat values; each pass is a no-op on the other shape.
void maskAll(JsonObject root, const std::vector<String>& keys);

// Drop secret keys carrying the mask, at the top level and inside the
// {settings:{...}} envelope the updaters also accept.
void stripMasked(JsonObject in, const std::vector<String>& keys);

}  // namespace SecretFields

#endif  // ESPRACK_SECRET_FIELDS_H
