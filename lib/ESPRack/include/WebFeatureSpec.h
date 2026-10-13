#ifndef WebFeatureSpec_h
#define WebFeatureSpec_h

#include <cstddef>
#include <cstdio>
#include <functional>
#include <vector>

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WebAuth.h>

// Canonical API prefix for the whole device HTTP surface (C1, api-v1).
// Every machine/UI endpoint the registry derives lives under this — the
// typed-feature form/state split (/api/v1/<id>/form, /api/v1/<id>/state),
// actions (/api/v1/actions/<id>), and the bespoke page paths the modules
// author. The old "/rest" prefix is gone entirely (pre-release clean cut,
// no aliases). Bespoke handlers that build their own paths should compose
// them from this constant so a future bump is a single edit.
static constexpr const char* API_V1_PREFIX = "/api/v1";

// Derive an action's canonical mount path (C1): always
// "/api/v1/actions/<id>" — dotted ids stay dotted (e.g.
// /api/v1/actions/wifi.scan). spec.restPath is intentionally ignored.
// Writes into the caller's buffer so callers can hand it straight to
// ArduinoJson's char[N] (Copy) overload or AsyncWebServer's String ctor
// without a dangling const char*. Shared by WebActionEntry (mount + proxy)
// and the manifest emitters in WebFeatureEntry / WebPageEntry.
inline void fillActionPath(char* buf, size_t buf_sz, const char* id) {
  if (buf_sz == 0) return;
  snprintf(buf, buf_sz, "%s/actions/%s", API_V1_PREFIX, id && *id ? id : "unknown");
}

struct WebMenuMeta {
  const char* label = nullptr;
  const char* icon = nullptr;
  int order = 0;
  WebAuthLevel auth = WebAuthLevel::Authenticated;
  bool hidden = false;
  // ── Tree-menu hierarchy ──
  // parent: id of the feature/group this item nests UNDER in the sidebar
  // tree. null = top-level. The frontend groups children by this and
  // renders a recursive, collapsible tree. The backend is the single
  // source of truth for menu placement — "drop X onto Y" == set X's
  // menu.parent = "Y". Arbitrary depth (the parent can have its own parent).
  const char* parent = nullptr;
  // expandTabs: when true, this feature's own tabs are ALSO exposed as
  // leaf children in the tree (navigate straight to /<id>/<tabKey>), in
  // addition to any child features. ON BY DEFAULT — every multi-tab feature
  // surfaces its tabs in the menu tree. Opt a feature OUT with
  // WebMenu(...).expandTabs(false) when its top tab-bar alone is preferred.
  // (Single-tab features are unaffected — the frontend only expands tabs>1.)
  bool expandTabs = true;
  // role: marks this as the device's MAIN-ROLE feature (the consumer app,
  // e.g. Light). In the device discovery menu the role feature(s) render
  // directly under the device record; everything else falls into the "Device" group.
  // Default false (framework / infra). The consumer opts its app feature in.
  bool role = false;
};

struct WebTabSpec {
  const char* key = nullptr;
  const char* title = nullptr;
  const char* icon = nullptr;   // optional MUI icon for the menu sub-tab leaf (expandTabs)
  // Endpoint the tab reads/writes. For a typed feature's OWN tabs leave this
  // null — toJson then emits the feature's derived form path
  // (/api/v1/<id>/form). For tabs CONTRIBUTED via addTabToFeature whose
  // backing service is itself a TYPED feature, set `formOf` below instead
  // of duplicating the derived path by hand. Only truly bespoke pages
  // (raw server->on handlers: fs, upload, system/* forms) author restPath
  // as a string — see the C1 PATH TABLE.
  const char* restPath = nullptr;
  // Id of a typed feature whose DERIVED form path this tab addresses.
  // Emitters resolve it to /api/v1/<formOf>/form at manifest build time,
  // so a feature-id rename can never silently desync a contributed tab.
  // Takes precedence over restPath when both are set.
  const char* formOf = nullptr;
  bool postable = false;  // tab can POST updates to restPath
  bool live = false;      // tab subscribes to feature.wsPath for push updates
  WebAuthLevel auth = WebAuthLevel::Authenticated;
  // Server-side WRITE gate for the keys rendered in this tab (LIB-FIX-011):
  // POST /state and WS writes touching such a key need at least this level.
  // Unset = the tab's `auth`, so an admin-only Settings tab is admin-write.
  bool hasWriteAuth = false;
  WebAuthLevel writeAuth = WebAuthLevel::Authenticated;
  // Sort key for tab placement within a feature (smaller = earlier).
  // For top-level features the tab vector is built directly inside
  // registerManifest, so insertion order matches code order — `order`
  // is mostly relevant for compound features where multiple modules
  // contribute via addTabToFeature() and topological install order
  // would otherwise produce a non-deterministic sequence.
  int order = 0;
};

struct WebActionSpec {
  const char* id = nullptr;
  const char* title = nullptr;
  const char* icon = nullptr;
  const char* color = nullptr;     // MUI color variant
  // DEPRECATED (C1): the action mount path is ALWAYS derived as
  // "/api/v1/actions/<id>" (dotted ids stay dotted, e.g.
  // /api/v1/actions/wifi.scan). This field is no longer consulted for
  // mounting, the manifest, or rest.proxy matching — kept only so module
  // specs that still set it compile. Remove once all modules drop it.
  const char* restPath = nullptr;
  const char* method = "POST";     // HTTP method
  WebAuthLevel auth = WebAuthLevel::Admin;
  std::function<void(AsyncWebServerRequest*)> handler;
  const char* confirm = nullptr;   // dialog text; null = fire immediately
  const char* successMessage = nullptr;

  // Optional in-process handler — bypasses HTTP/AsyncWebServer when
  // the action is invoked via Mothership's rest.proxy machinery
  // (Phase 2). Same auth context as the parent mTLS check-in — we
  // trust the server queue, not the JWT we don't have. When unset,
  // the rest.proxy action returns "not handled" for this path.
  // For most modules where the local-UI `handler` only calls
  // r->send(200, ...), the internalHandler is trivially:
  //   spec.internalHandler = [this](JsonVariantConst in, JsonVariant out, int& status) {
  //     // do the real work
  //     out["ok"] = true; status = 200;
  //   };
  std::function<void(JsonVariant     /*in*/,
                      JsonVariant     /*out*/,
                      int&            /*status*/)> internalHandler;
};

struct WebEndpointMeta {
  const char* method = "GET";
  const char* path = nullptr;
  WebAuthLevel auth = WebAuthLevel::Authenticated;
  const char* role = nullptr;

  // ── Handlers ──
  //
  // Pick whichever shape matches the endpoint; WebPageEntry's
  // registerEndpoints walks the spec and binds the right one:
  //
  //   handler         — plain GET (or POST without auto-parsed body)
  //                     receives only the request, must do its own
  //                     query / body parsing. Default registration:
  //                     server->on(path, METHOD, wrap(handler))
  //
  //   jsonHandler     — POST / PUT / PATCH with a JSON body. Wrapped
  //                     in an AsyncCallbackJsonWebHandler so the
  //                     handler sees both the request and a parsed
  //                     JsonVariant. Saves boilerplate body buffering.
  //
  //   internalHandler — in-process invocation for Mothership's
  //                     rest.proxy. Bypasses AsyncWebServer entirely
  //                     so the same operation works without HTTP plumbing
  //                     (no JWT check, body is a JsonVariant the action
  //                     handler already parsed). Status/body written
  //                     to the out parameters.
  //
  // Setting `handler` AND `jsonHandler` is a misconfiguration — the
  // first one set wins, jsonHandler preferred for body-aware methods.
  // `internalHandler` is independent and works alongside either of
  // the other two (the local-browser path uses handler/jsonHandler,
  // the mothership path uses internalHandler).
  std::function<void(AsyncWebServerRequest*)> handler;
  std::function<void(AsyncWebServerRequest*, JsonVariant&)> jsonHandler;
  std::function<void(JsonVariant /*in*/,
                      JsonVariant /*out*/,
                      int&        /*status*/)> internalHandler;
};

struct WebFeatureSpec {
  const char* id = nullptr;
  const char* title = nullptr;
  // Service-implementation version (semver). Distinct from the wrapping
  // Module's version — the module wrapper may stay at 1.0.0 while the
  // underlying service iterates faster. Optional; null = not reported.
  // Surfaced on the System / Endpoints page so operators can audit
  // which exact service rev is responding on a given endpoint.
  const char* version = nullptr;
  const char* component = "DynamicSettings";
  WebMenuMeta menu;
  WebAuthLevel auth = WebAuthLevel::Authenticated;
  // Floor for EVERY write of a typed feature (LIB-FIX-011): POST /state and
  // WS writes need max(auth, writeAuth, the touched keys' tab levels).
  // Unset = `auth`.
  bool hasWriteAuth = false;
  WebAuthLevel writeAuth = WebAuthLevel::Authenticated;
  const char* routeTemplate = nullptr;
  // DEPRECATED for typed features (C1): WebFeatureEntry now DERIVES its mount
  // paths from `id` — form at /api/v1/<id>/form, state at /api/v1/<id>/state —
  // and ignores these two fields. Kept so existing module specs that still set
  // restRead/restUpdate compile unchanged; remove from those specs over time.
  // (WebInfoEntry — the metadata/info-feature path — STILL honours restRead as
  // its single GET mount point; see WebInfoEntry.h.)
  const char* restRead = nullptr;
  const char* restUpdate = nullptr;
  const char* wsPath = nullptr;
  std::vector<WebTabSpec> tabs;
  std::vector<WebActionSpec> actions;
};

struct WebPageSpec {
  const char* id = nullptr;
  const char* title = nullptr;
  const char* version = nullptr;  // optional, semver — see WebFeatureSpec::version
  const char* component = nullptr;
  WebMenuMeta menu;
  WebAuthLevel auth = WebAuthLevel::Authenticated;
  const char* routeTemplate = nullptr;
  std::vector<WebEndpointMeta> endpoints;
  std::vector<WebActionSpec> actions;
};

#endif
