#ifndef WebFeatureSpec_h
#define WebFeatureSpec_h

#include <functional>
#include <vector>

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WebAuth.h>

struct WebMenuMeta {
  const char* label = nullptr;
  const char* icon = nullptr;
  int order = 0;
  WebAuthLevel auth = WebAuthLevel::Authenticated;
  bool hidden = false;
};

struct WebTabSpec {
  const char* key = nullptr;
  const char* title = nullptr;
  const char* restPath = nullptr;
  bool postable = false;  // tab can POST updates to restPath
  bool live = false;      // tab subscribes to feature.wsPath for push updates
  WebAuthLevel auth = WebAuthLevel::Authenticated;
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
  const char* restPath = nullptr;  // if null, auto: "/rest/action/<id>"
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
  const char* routeTemplate = nullptr;
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
