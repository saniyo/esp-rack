#ifndef WebManager_h
#define WebManager_h

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>

#include <SecurityManager.h>
#include <WsManager.h>
#include <IWebFeatureEntry.h>
#include <WebFeatureDelegate.h>
#include <WebPageEntry.h>
#include <WebActionEntry.h>
#include <WebInfoEntry.h>

#define UI_MANIFEST_PATH "/rest/uiManifest"
// Manifest assembly streams via AsyncResponseStream + per-entry
// serialization (see WebManager.cpp::serveManifest), so there is no
// aggregate buffer to size. Adding modules is free in terms of
// transport memory — the only thing that has to fit a budget is a
// SINGLE feature's serialized spec, capped to 4 KB inside serveManifest.

class WebManager {
 public:
  WebManager(AsyncWebServer* server,
             SecurityManager* sm,
             WsManager* wsManager,
             const char* deviceName,
             const char* deviceVersion);

  // Mount /rest/uiManifest and call registerEndpoints() on every registered entry.
  // Call once from ESPReact::begin() AFTER all services have registered.
  void begin();

  // Owning registration. Returns a raw pointer so services may keep a handle.
  template <typename T>
  T* registerEntry(std::unique_ptr<T> entry) {
    static_assert(std::is_base_of<IWebFeatureEntry, T>::value,
                  "WebManager::registerEntry requires IWebFeatureEntry subclass");
    T* raw = entry.get();
    _entries.push_back(std::move(entry));
    return raw;
  }

  // Typed-feature helper. Same reader/updater is used for REST and WS.
  template <typename T>
  WebFeatureEntry<T>* registerFeature(WebFeatureSpec spec,
                                      StatefulService<T>* service,
                                      JsonStateReader<T> reader,
                                      JsonStateUpdater<T> updater,
                                      size_t bufferSize = DEFAULT_BUFFER_SIZE) {
    auto entry = std::unique_ptr<WebFeatureEntry<T>>(
        new WebFeatureEntry<T>(std::move(spec), service, reader, updater, _wsManager, bufferSize));
    return registerEntry(std::move(entry));
  }

  // Typed-feature helper with separate REST and WS reader/updater. Passing a
  // single bufferSize applies to both REST and WS; use the four-arg overload
  // below when they need different allocations (e.g. a FormBuilder REST
  // response that's much larger than the plain-state WS push).
  template <typename T>
  WebFeatureEntry<T>* registerFeature(WebFeatureSpec spec,
                                      StatefulService<T>* service,
                                      JsonStateReader<T> restReader,
                                      JsonStateUpdater<T> restUpdater,
                                      JsonStateReader<T> wsReader,
                                      JsonStateUpdater<T> wsUpdater,
                                      size_t bufferSize = DEFAULT_BUFFER_SIZE) {
    return registerFeature<T>(std::move(spec), service, restReader, restUpdater,
                              wsReader, wsUpdater, bufferSize, bufferSize);
  }

  // Four-arg overload: separate REST and WS buffer sizes.
  template <typename T>
  WebFeatureEntry<T>* registerFeature(WebFeatureSpec spec,
                                      StatefulService<T>* service,
                                      JsonStateReader<T> restReader,
                                      JsonStateUpdater<T> restUpdater,
                                      JsonStateReader<T> wsReader,
                                      JsonStateUpdater<T> wsUpdater,
                                      size_t restBufferSize,
                                      size_t wsBufferSize) {
    auto entry = std::unique_ptr<WebFeatureEntry<T>>(
        new WebFeatureEntry<T>(std::move(spec), service,
                               restReader, restUpdater,
                               wsReader, wsUpdater,
                               _wsManager, restBufferSize, wsBufferSize));
    return registerEntry(std::move(entry));
  }

  // Manifest-only page registration for services that own raw handlers.
  WebPageEntry* registerPage(WebPageSpec spec) {
    auto entry = std::unique_ptr<WebPageEntry>(new WebPageEntry(std::move(spec)));
    return registerEntry(std::move(entry));
  }

  // Top-level action: mounts a single HTTP handler + publishes the action
  // in the manifest under kind="action". Forms embed it via actionRef("id");
  // standalone bars iterate manifest features filtered by kind.
  WebActionEntry* registerAction(WebActionSpec spec) {
    auto entry = std::unique_ptr<WebActionEntry>(new WebActionEntry(std::move(spec)));
    return registerEntry(std::move(entry));
  }

  // Stateless read-only feature — reader lambda emits a form directly into
  // the response JsonObject at spec.restRead. No StatefulService, no WS.
  WebInfoEntry* registerInfoFeature(WebFeatureSpec spec,
                                    std::function<void(JsonObject&)> reader,
                                    size_t bufferSize = DEFAULT_BUFFER_SIZE) {
    auto entry = std::unique_ptr<WebInfoEntry>(
        new WebInfoEntry(std::move(spec), std::move(reader), bufferSize));
    return registerEntry(std::move(entry));
  }

  // Metadata-only feature — no endpoint is mounted. Use for compound pages
  // whose tabs point at restPaths owned by other services (e.g. 'system'
  // whose status/ota/upload tabs are served by three separate services).
  WebInfoEntry* registerCompoundFeature(WebFeatureSpec spec) {
    auto entry = std::unique_ptr<WebInfoEntry>(new WebInfoEntry(std::move(spec)));
    return registerEntry(std::move(entry));
  }

  // Build-feature flags exposed to the frontend via manifest.buildFeatures.
  void registerBuildFeature(const char* key, bool enabled) {
    _buildFeatures.push_back({key, enabled});
  }

  // Module-version registry. App.begin() iterates installed modules,
  // calls describe() on each, and pushes (id, version) pairs here so
  // /rest/uiManifest.modules[] surfaces the wrapper rev independent
  // of any per-feature WebFeatureSpec.version. Pointers are NOT
  // copied — caller must guarantee stable lifetime (typical: string
  // literals embedded in module source, or the descriptor's id/version
  // const char* stored statically).
  void registerModule(const char* id, const char* version) {
    if (!id) return;
    _modules.push_back({id, version ? version : ""});
  }

  // Read-only iteration over the module-version registry. Used by
  // SystemStatus to render the "Modules" table on the System tab —
  // same constraint as forEachEntry: never call register*() from
  // inside the callback.
  template <typename Fn>
  void forEachModule(Fn cb) const {
    for (const auto& m : _modules) cb(m.id, m.version);
  }

  // Framework version setter — App.cpp calls this once during begin()
  // with ESPRACK_VERSION_STR so the manifest's device block carries
  // the library rev independent of consumer-app versioning.
  void setFrameworkVersion(const char* v) { _frameworkVersion = v ? v : ""; }
  const char* frameworkVersion() const { return _frameworkVersion; }

  // ---- public iteration over registered entries ----
  // Used by WebEndpointsService to render the per-entry table in the
  // System → Endpoints tab. The callback gets a const-ref because the
  // existing IWebFeatureEntry surface (id / kind / toJson) is read-only.
  // Mutation of the entry list happens during early init only (services
  // call register* from their ctors), so no locking is needed; callers
  // MUST NOT touch register*() from inside the callback.
  template <typename Fn>
  void forEachEntry(Fn cb) const {
    for (const auto& e : _entries) if (e) cb(*e);
  }

  // Late-bind the SecurityManager pointer. Used by the SecurityModule
  // to swap App's NullSecurityManager for the real PBKDF2-backed
  // implementation during onInstall. Must be called BEFORE any other
  // module's onInstall (priority 5 ensures this) — entries registered
  // before this point may have captured the old pointer in lambdas.
  void setSecurityManager(SecurityManager* sm) { _sm = sm; }

  // Append a tab to an existing feature entry. Primary use case: compound
  // features (e.g. the 'system' shell) filled in by multiple services —
  // each service declares its own tab in its registerManifest instead of a
  // single aggregator (ESPReact) knowing every tab's details.
  // Returns true if the target feature was found and accepted the tab.
  bool addTabToFeature(const char* featureId, const WebTabSpec& tab) {
    if (!featureId) return false;
    for (auto& e : _entries) {
      if (!e) continue;
      if (std::strcmp(e->id(), featureId) == 0) {
        return e->addTab(tab);
      }
    }
    return false;
  }

  // Phase 2 — In-process REST dispatch for Mothership's rest.proxy.
  // Walks every registered IWebFeatureEntry until one claims the
  // (method, path) pair. On match it populates out_status + out_body
  // and returns true; on miss the caller surfaces a 404 to the
  // upstream operator (mothership UI reads it from action_results).
  //
  // Caller pre-allocates the document backing out_body. Typical
  // pattern in MothershipService::actionRestProxy:
  //   DynamicJsonDocument respDoc(8192);
  //   JsonVariant out = respDoc.to<JsonVariant>();
  //   int status = 404;
  //   bool ok = _web->proxyDispatch(method, path, body, status, out);
  //   if (!ok) status = 404;
  //
  // Auth note: this bypasses AsyncWebServer's predicate chain on
  // purpose — the request's trust comes from the mTLS handshake at
  // /api/v1/checkin, NOT from a JWT we don't have. Per-entry impls
  // refuse to call internalHandler if the spec didn't provide one,
  // so a module that hasn't opted-in to the proxy mechanism stays
  // unreachable.
  bool proxyDispatch(const char* method,
                      const char* path,
                      JsonVariant body,
                      int& out_status,
                      JsonVariant out_body) {
    if (!method || !path) {
      out_status = 400;
      return false;
    }
    for (auto& e : _entries) {
      if (!e) continue;
      if (e->proxyDispatch(method, path, body, out_status, out_body)) {
        return true;
      }
    }
    return false;
  }

 private:
  void serveManifest(AsyncWebServerRequest* request);

  AsyncWebServer* _server{nullptr};
  SecurityManager* _sm{nullptr};
  WsManager* _wsManager{nullptr};
  const char* _deviceName{""};
  const char* _deviceVersion{""};

  std::vector<std::unique_ptr<IWebFeatureEntry>> _entries;

  struct BuildFeature {
    const char* key;
    bool enabled;
  };
  std::vector<BuildFeature> _buildFeatures;

  struct ModuleVersion {
    const char* id;
    const char* version;
  };
  std::vector<ModuleVersion> _modules;

  const char* _frameworkVersion{""};

  bool _begun{false};
};

#endif
