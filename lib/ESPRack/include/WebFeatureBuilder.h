#ifndef WebFeatureBuilder_h
#define WebFeatureBuilder_h

#include <utility>

#include <WebFeatureSpec.h>
#include <WebAuth.h>

// ─────────────────────────────────────────────────────────────────────────
//  Fluent, type-safe builders for WebFeatureSpec / WebTabSpec / WebActionSpec.
//
//  WHY: the raw structs are easy to mis-fill. Every value is a bare field you
//  can typo with NO compile error and NO IDE guidance — `menu.parent = "wfi"`
//  silently orphans the item; a misspelled `icon` / `component` renders a
//  broken UI. These builders make authoring:
//    • DISCOVERABLE — type `WebFeature("x").` and the editor lists every
//      option; `WebIcon::` / `WebComponent::` list every known value;
//    • HARD TO GET WRONG — the id is required in the ctor, auth/visibility is
//      a named method, menu placement is `.nestUnder(parentId)`, and the
//      common value sets are named constants instead of magic strings.
//
//  ADDITIVE: the structs are untouched and struct-style registration keeps
//  working — this is just a nicer front door that yields the very same
//  WebFeatureSpec. The wire format is unchanged: builders shape only the
//  C++-side authoring API; toJson still emits the identical manifest JSON.
//
//  USAGE:
//    auto spec = WebFeature("lightState")
//        .title("Light Service")
//        .component(WebComponent::DynamicSettings)
//        .menu(WebMenu("Light").icon(WebIcon::Lightbulb).order(50))  // tabs expand by default
//        .rest(LIGHT_SETTINGS_PATH)
//        .ws(LIGHT_SETTINGS_SOCKET_PATH)
//        .tab(WebTab("status", "Status").rest(LIGHT_SETTINGS_PATH).live())
//        .tab(WebTab("settings", "Settings").rest(LIGHT_SETTINGS_PATH).postable())
//        .build();
//    web->registerFeature<LightState>(std::move(spec), this, ...);
// ─────────────────────────────────────────────────────────────────────────

// Frontend dynamic-renderer component identifiers. Use instead of raw strings
// so a typo is a missing-symbol compile error rather than a blank page.
namespace WebComponent {
constexpr const char* DynamicSettings = "DynamicSettings";
constexpr const char* DynamicStatus   = "DynamicStatus";
constexpr const char* DynamicFeature  = "DynamicFeature";
}  // namespace WebComponent

// Curated MUI icon names actually referenced in-tree. NOT exhaustive — any
// valid MUI icon name still works as a raw string; these are the typo-proof
// shortcuts for the common ones. (Long-term these should be GENERATED from
// the frontend icon registry so C++ and TS can't drift — see
// docs/plans/tree-menu.md.)
namespace WebIcon {
constexpr const char* Lightbulb             = "Lightbulb";
constexpr const char* Wifi                  = "Wifi";
constexpr const char* SettingsInputAntenna  = "SettingsInputAntenna";
constexpr const char* Settings              = "Settings";
constexpr const char* Lock                  = "Lock";
constexpr const char* Cloud                 = "Cloud";
constexpr const char* Folder                = "Folder";
constexpr const char* Hub                   = "Hub";
constexpr const char* AccessTime            = "AccessTime";
constexpr const char* Send                  = "Send";
constexpr const char* Refresh               = "Refresh";
constexpr const char* SettingsBackupRestore = "SettingsBackupRestore";
constexpr const char* NetworkPing           = "NetworkPing";
constexpr const char* PlayArrow             = "PlayArrow";
}  // namespace WebIcon

// ── Menu sub-builder ──────────────────────────────────────────────────────
// Wraps WebMenuMeta. Implicitly converts to WebMenuMeta, so it drops straight
// into WebFeature::menu(...) without an explicit .build().
class WebMenu {
 public:
  explicit WebMenu(const char* label) { _m.label = label; }

  WebMenu& icon(const char* i)  { _m.icon = i; return *this; }
  WebMenu& order(int o)         { _m.order = o; return *this; }
  WebMenu& hidden(bool v = true){ _m.hidden = v; return *this; }

  // Visibility helpers (default is Authenticated — the struct default).
  WebMenu& auth(WebAuthLevel a) { _m.auth = a; return *this; }
  WebMenu& publicAccess()       { _m.auth = WebAuthLevel::Public; return *this; }
  WebMenu& authenticated()      { _m.auth = WebAuthLevel::Authenticated; return *this; }
  WebMenu& adminOnly()          { _m.auth = WebAuthLevel::Admin; return *this; }

  // Tree placement — "drop this onto <parentId>": nest this item UNDER the
  // feature/group whose id == parentId. Omit for a top-level item.
  WebMenu& nestUnder(const char* parentId) { _m.parent = parentId; return *this; }
  // A page of external HW instance <instanceId>; use ExtHwModule::hwMenu().
  WebMenu& hw(const char* instanceId)      { _m.hw = instanceId; return *this; }
  // Surface this feature's own tabs as leaf children of the tree node.
  // ON BY DEFAULT (WebMenuMeta::expandTabs == true); call expandTabs(false)
  // to opt OUT and keep this feature's tabs in the top tab-bar only.
  WebMenu& expandTabs(bool v = true)       { _m.expandTabs = v; return *this; }
  // Mark this as the device's MAIN-ROLE feature (the consumer app). In the
  // device discovery menu it renders directly under the device record; non-role
  // features fall into the "Device" group. Default off (framework / infra).
  WebMenu& role(bool v = true)             { _m.role = v; return *this; }

  WebMenuMeta build() const  { return _m; }
  operator WebMenuMeta() const { return _m; }

 private:
  WebMenuMeta _m;
};

// ── Tab sub-builder ───────────────────────────────────────────────────────
class WebTab {
 public:
  explicit WebTab(const char* key, const char* title = nullptr) {
    _t.key = key;
    _t.title = title;
  }

  WebTab& icon(const char* i)     { _t.icon = i; return *this; }
  WebTab& rest(const char* path)  { _t.restPath = path; return *this; }
  WebTab& postable(bool v = true) { _t.postable = v; return *this; }
  WebTab& live(bool v = true)     { _t.live = v; return *this; }
  WebTab& order(int o)            { _t.order = o; return *this; }

  WebTab& auth(WebAuthLevel a)    { _t.auth = a; return *this; }
  WebTab& publicAccess()          { _t.auth = WebAuthLevel::Public; return *this; }
  WebTab& authenticated()         { _t.auth = WebAuthLevel::Authenticated; return *this; }
  WebTab& adminOnly()             { _t.auth = WebAuthLevel::Admin; return *this; }

  // Server-enforced write level for this tab's keys (default: the tab's
  // auth). E.g. a tab everyone may SEE but only admins may CHANGE:
  //   WebTab("settings").authenticated().adminWrite()
  WebTab& writeAuth(WebAuthLevel a) { _t.hasWriteAuth = true; _t.writeAuth = a; return *this; }
  WebTab& adminWrite()              { return writeAuth(WebAuthLevel::Admin); }

  WebTabSpec build() const   { return _t; }
  operator WebTabSpec() const { return _t; }

 private:
  WebTabSpec _t;
};

// ── Action sub-builder ────────────────────────────────────────────────────
class WebAction {
 public:
  explicit WebAction(const char* id, const char* title = nullptr) {
    _a.id = id;
    _a.title = title;
  }

  WebAction& icon(const char* i)    { _a.icon = i; return *this; }
  WebAction& color(const char* c)   { _a.color = c; return *this; }
  // DEPRECATED (C1): the action path is always derived as
  // /api/v1/actions/<id>; restPath is ignored. Kept for compile-compat.
  WebAction& rest(const char* path) { _a.restPath = path; return *this; }
  WebAction& method(const char* m)  { _a.method = m; return *this; }
  WebAction& confirm(const char* text) { _a.confirm = text; return *this; }
  WebAction& success(const char* msg)  { _a.successMessage = msg; return *this; }

  WebAction& auth(WebAuthLevel a)   { _a.auth = a; return *this; }
  WebAction& publicAccess()         { _a.auth = WebAuthLevel::Public; return *this; }
  WebAction& authenticated()        { _a.auth = WebAuthLevel::Authenticated; return *this; }
  WebAction& adminOnly()            { _a.auth = WebAuthLevel::Admin; return *this; }

  // Local-browser handler (the usual r->send(...) path).
  WebAction& handler(std::function<void(AsyncWebServerRequest*)> h) {
    _a.handler = std::move(h);
    return *this;
  }
  // In-process handler for Mothership rest.proxy (no HTTP / no JWT).
  WebAction& internalHandler(
      std::function<void(JsonVariant, JsonVariant, int&)> h) {
    _a.internalHandler = std::move(h);
    return *this;
  }

  WebActionSpec build() const   { return _a; }
  operator WebActionSpec() const { return _a; }

 private:
  WebActionSpec _a;
};

// ── Feature builder ───────────────────────────────────────────────────────
// The main entry point. id is required (ctor). Call .build() to hand the
// finished WebFeatureSpec to web->registerFeature(...).
class WebFeature {
 public:
  explicit WebFeature(const char* id) { _spec.id = id; }

  WebFeature& title(const char* t)     { _spec.title = t; return *this; }
  WebFeature& version(const char* v)   { _spec.version = v; return *this; }
  WebFeature& component(const char* c) { _spec.component = c; return *this; }
  WebFeature& route(const char* tmpl)  { _spec.routeTemplate = tmpl; return *this; }

  // Accepts a WebMenu (implicitly) or a raw WebMenuMeta.
  WebFeature& menu(const WebMenuMeta& m) { _spec.menu = m; return *this; }

  // Feature-wide auth gate (default Authenticated).
  WebFeature& auth(WebAuthLevel a) { _spec.auth = a; return *this; }
  WebFeature& publicAccess()       { _spec.auth = WebAuthLevel::Public; return *this; }
  WebFeature& authenticated()      { _spec.auth = WebAuthLevel::Authenticated; return *this; }
  WebFeature& adminOnly()          { _spec.auth = WebAuthLevel::Admin; return *this; }

  // Floor for every write of this feature (server-enforced; default: auth).
  WebFeature& writeAuth(WebAuthLevel a) { _spec.hasWriteAuth = true; _spec.writeAuth = a; return *this; }
  WebFeature& adminWrite()              { return writeAuth(WebAuthLevel::Admin); }

  // DEPRECATED (C1): WebFeatureEntry now DERIVES typed-feature paths from the
  // id (/api/v1/<id>/form, /api/v1/<id>/state) and ignores restRead/restUpdate.
  // These setters remain so existing builder chains compile, but the paths
  // they set are no longer mounted. Drop the .rest()/.readOnly() calls from
  // feature builders over time.
  // GET + POST share one path (the common FormBuilder case).
  WebFeature& rest(const char* path) {
    _spec.restRead = path;
    _spec.restUpdate = path;
    return *this;
  }
  // Separate read / update endpoints.
  WebFeature& rest(const char* read, const char* update) {
    _spec.restRead = read;
    _spec.restUpdate = update;
    return *this;
  }
  // Read-only: only a GET endpoint, no POST.
  WebFeature& readOnly(const char* read) { _spec.restRead = read; return *this; }

  WebFeature& ws(const char* path) { _spec.wsPath = path; return *this; }

  // Accepts a WebTab (implicitly) or a raw WebTabSpec. Order = code order.
  WebFeature& tab(const WebTabSpec& t) { _spec.tabs.push_back(t); return *this; }
  // Accepts a WebAction (implicitly) or a raw WebActionSpec.
  WebFeature& action(const WebActionSpec& a) { _spec.actions.push_back(a); return *this; }

  // Moves the finished spec out — the builder is single-use (it lives only
  // for the duration of the chained expression, which is the intended usage).
  WebFeatureSpec build() { return std::move(_spec); }

 private:
  WebFeatureSpec _spec;
};

#endif  // WebFeatureBuilder_h
