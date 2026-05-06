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
#define UI_MANIFEST_BUFFER_SIZE 8192

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

  bool _begun{false};
};

#endif
