#include "WriteAuth.h"

#include <string.h>

#include "WebFeatureSpec.h"

namespace {
WebAuthLevel maxLevel(WebAuthLevel a, WebAuthLevel b) {
  return WriteAuthMap::atLeast(a, b) ? a : b;
}
}  // namespace

void WriteAuthMap::build(JsonObjectConst formRoot, const std::vector<WebTabSpec>& tabs,
                         WebAuthLevel featureAuth, WebAuthLevel featureWriteAuth) {
  _keys.clear();
  _tabs.clear();
  _featureAuth = featureAuth;
  _floor = maxLevel(featureAuth, featureWriteAuth);
  _elevated = !atLeast(featureAuth, _floor);

  for (const WebTabSpec& t : tabs) {
    if (!t.key) continue;
    const WebAuthLevel tl =
        maxLevel(_floor, t.hasWriteAuth ? t.writeAuth : t.auth);
    _tabs.push_back({String(t.key), tl});
    if (!atLeast(featureAuth, tl)) _elevated = true;

    // The tab's form: formRoot[tabKey] = { description, fields: [ {<key>: v, "o": ...}, ... ] }
    JsonObjectConst form = formRoot[t.key].as<JsonObjectConst>();
    if (form.isNull()) continue;
    JsonArrayConst fields = form["fields"].as<JsonArrayConst>();
    for (JsonVariantConst fv : fields) {
      JsonObjectConst f = fv.as<JsonObjectConst>();
      if (f.isNull()) continue;
      for (JsonPairConst kv : f) {
        const char* k = kv.key().c_str();
        if (strcmp(k, "o") == 0) continue;
        bool found = false;
        for (KeyLevel& e : _keys) {
          if (e.key == k) {
            e.level = maxLevel(e.level, tl);
            found = true;
            break;
          }
        }
        if (!found) _keys.push_back({String(k), tl});
        break;  // exactly one value key per field
      }
    }
  }

  // No form (probe couldn't allocate / feature has no reader): keys can't
  // be mapped, so fail CLOSED — every write needs the strictest tab level.
  if (formRoot.isNull()) {
    for (const TabLevel& t : _tabs) _floor = maxLevel(_floor, t.level);
    _elevated = !atLeast(featureAuth, _floor);
  }
}

const WriteAuthMap::TabLevel* WriteAuthMap::tab(const char* key) const {
  for (const TabLevel& t : _tabs) {
    if (t.key == key) return &t;
  }
  return nullptr;
}

WebAuthLevel WriteAuthMap::levelOfKey(const char* key) const {
  for (const KeyLevel& e : _keys) {
    if (e.key == key) return e.level;
  }
  return _floor;
}

WebAuthLevel WriteAuthMap::required(JsonObjectConst body) const {
  WebAuthLevel need = _floor;
  for (JsonPairConst kv : body) {
    const char* k = kv.key().c_str();
    const TabLevel* t = tab(k);
    if (t && kv.value().is<JsonObjectConst>()) {
      // {"settings": {...}} envelope: the tab's level + each inner key's.
      need = maxLevel(need, t->level);
      for (JsonPairConst inner : kv.value().as<JsonObjectConst>()) {
        need = maxLevel(need, levelOfKey(inner.key().c_str()));
      }
    } else {
      need = maxLevel(need, levelOfKey(k));
    }
  }
  return need;
}
