#ifndef WebFeatureSpec_h
#define WebFeatureSpec_h

#include <functional>
#include <vector>

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
};

struct WebEndpointMeta {
  const char* method = "GET";
  const char* path = nullptr;
  WebAuthLevel auth = WebAuthLevel::Authenticated;
  const char* role = nullptr;
};

struct WebFeatureSpec {
  const char* id = nullptr;
  const char* title = nullptr;
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
  const char* component = nullptr;
  WebMenuMeta menu;
  WebAuthLevel auth = WebAuthLevel::Authenticated;
  const char* routeTemplate = nullptr;
  std::vector<WebEndpointMeta> endpoints;
  std::vector<WebActionSpec> actions;
};

#endif
