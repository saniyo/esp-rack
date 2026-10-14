#ifndef WebManager_h
#define WebManager_h

#include <cstring>

// SES-LIB-030: soft budget of one cached manifest tier (raw bytes). Over it:
// a warning at boot and in System -> Status.
#ifndef ESPRACK_MANIFEST_BUDGET
#define ESPRACK_MANIFEST_BUDGET 16384
#endif
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <SecurityManager.h>
#include <WsManager.h>
#include <IWebFeatureEntry.h>
#include <WebFeatureDelegate.h>
#include <WebPageEntry.h>
#include <WebActionEntry.h>
#include <WebInfoEntry.h>

#define UI_MANIFEST_PATH "/api/v1/ui/manifest"
// Manifest assembly streams JSON on the fly via ManifestBuilder (a
// stateful section-by-section generator owned as a WebManager member
// in BSS). One builder serves one request at a time, gated by a
// FreeRTOS mutex — a second concurrent fetch gets 503 "retry later".
// Zero per-request heap allocation; no allocator can throw bad_alloc.
class WebManager;

// Section-by-section JSON builder for /api/v1/ui/manifest. Lives as a
// WebManager member (BSS), not a per-request heap allocation. Reset
// at the start of each manifest fetch via reset(mgr, authed); the
// AwsResponseFiller in WebManager::serveManifest drives fillNext()
// until it returns 0 (response complete). Mutex serialises requests.
class ManifestBuilder {
 public:
  ManifestBuilder() : _mgr(nullptr), _authed(false) {}

  // Re-initialise for a fresh response. Call before the first
  // fillNext() call of each new request. Doesn't allocate; just
  // resets state and pre-loads the HEADER phase buffer.
  void reset(const WebManager& mgr, bool authed);

  // Fill up to maxLen bytes into buf. Returns bytes written.
  // Returns 0 when the response is complete (signals end to
  // AsyncWebServer's chunked responder).
  size_t fillNext(uint8_t* buf, size_t maxLen);

 private:
  enum class Phase : uint8_t {
    HEADER, DEVICE_OPEN, DEVICE,
    BUILDFEATURES_OPEN, BUILDFEATURES,
    AUTH_FALSE_END, AUTH_TRUE,
    MODULES_OPEN, MODULES_ITEM, MODULES_SEP, MODULES_CLOSE,
    FEATURES_OPEN, FEATURES_ITEM, FEATURES_SEP, FEATURES_CLOSE,
    EXTRA_OPEN, EXTRA_ITEM, EXTRA_SEP, EXTRA_CLOSE,
    FOOTER, DONE
  };

  void setLiteral(const char* s);
  void renderDocIntoItem();
  void advanceTo(Phase p);
  void advancePhase();
  void fillPhaseBuffer();

  const WebManager* _mgr;
  bool   _authed;
  Phase  _phase{Phase::DONE};
  size_t _modIdx{0};
  size_t _featIdx{0};
  size_t _extraIdx{0};
  size_t _cursor{0};
  size_t _itemLen{0};

  // 2 KB per-entry rendering buffer. Some feature entries (mothership
  // with action_results metadata, cert-manager with actions + tabs,
  // system shell with three contributed tabs) serialize to >1 KB.
  // Trying 1 KB silently truncated those entries' JSON mid-key,
  // produced malformed manifest, and the SPA fell back to the legacy
  // features list (rendered only "Security"). 2 KB matches empirical
  // worst case + headroom.
  char _itemBuf[2048];

  // ArduinoJson pool for per-entry construction. 2 KB holds the
  // internal slot/string metadata for typical entries (~15 keys, a
  // few nested objects); larger than the serialized output isn't
  // necessary since the pool only stores ArduinoJson's bookkeeping,
  // not the rendered text.
  StaticJsonDocument<2048> _doc;
};

class WebManager {
  friend class ManifestBuilder;

 public:
  WebManager(AsyncWebServer* server,
             SecurityManager* sm,
             WsManager* wsManager,
             const char* deviceName,
             const char* deviceVersion);

  // Mount the UI manifest endpoint and call registerEndpoints() on every registered entry.
  // Call once from ESPReact::begin() AFTER all services have registered.
  void begin();

  // Owning registration. Returns a raw pointer so services may keep a handle.
  template <typename T>
  T* registerEntry(std::unique_ptr<T> entry) {
    static_assert(std::is_base_of<IWebFeatureEntry, T>::value,
                  "WebManager::registerEntry requires IWebFeatureEntry subclass");
    T* raw = entry.get();
    warnIfDuplicate(*raw);
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

  // Query the install map populated by registerBuildFeature (App::begin
  // registers one entry per installed module, keyed by describe().id, plus
  // the consumer "project" flag). True iff `key` was registered enabled.
  // Lets services derive feature gating from the .install<>() chain instead
  // of per-module FT_* compile macros. Read-only and safe on the request
  // thread — the map is built once in begin() and never mutated after.
  bool hasBuildFeature(const char* key) const {
    if (!key) return false;
    for (const auto& f : _buildFeatures) {
      if (f.key && std::strcmp(f.key, key) == 0) return f.enabled;
    }
    return false;
  }

  // Module-version registry. App.begin() iterates installed modules,
  // calls describe() on each, and pushes (id, version) pairs here so
  // manifest.modules[] surfaces the wrapper rev independent
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

  // Re-bake the flash manifest cache (and its ETag) after a runtime change
  // the manifest shows — the device timezone (SES-LIB-015). No-op before
  // begin(). Loop task; rewrites only when the content signature changed.
  void refreshManifestCache();

  // SES-LIB-030: bytes of the cached manifest per tier (0 = not cached).
  uint32_t manifestBytes(bool authed) const { return _manifestBytes[authed ? 1 : 0]; }

  // A root-level manifest array that another core object fills, streamed
  // one item at a time like the features: the hardware registry's "extHw"
  // (SES-LIB-028). One source; a later call replaces it.
  struct ManifestArray {
    const char* key = nullptr;
    std::function<size_t()> count;
    std::function<void(size_t, JsonObject)> item;
  };
  void setManifestArray(ManifestArray a) { _extraArray = std::move(a); }
  // fn(featureId) for every entry whose menu.hw == hwId: an instance's pages.
  void forEachHwPage(const char* hwId, const std::function<void(const char*)>& fn) const;
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

  // Indexable access to the registry — for a RESUMABLE walk that survives
  // across calls (forEachEntry runs to completion in one shot and can't be
  // paused). Used by the endpoints-tab chunked streamer, which serializes
  // one entry per beginChunkedResponse callback so a big registry table
  // never has to exist as one contiguous tree. Index/count are stable
  // after boot (entries are only registered during init, never removed).
  size_t entryCount() const { return _entries.size(); }
  const IWebFeatureEntry* entryAt(size_t i) const {
    return (i < _entries.size()) ? _entries[i].get() : nullptr;
  }

  // Late-bind the SecurityManager pointer. Used by the SecurityModule
  // to swap App's NullSecurityManager for the real PBKDF2-backed
  // implementation during onInstall. Registry entries are safe either
  // way: registerEndpoints() runs in WebManager::begin(), after every
  // onInstall, and reads _sm then. Only code that captures ctx.security
  // during its OWN onInstall before SecurityModule (priority 8) sees the
  // Null stub - see Module.h.
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
    // Special case: the UI manifest path. The browser-facing handler
    // streams the body via AsyncResponseStream to keep heap flat,
    // but rest.proxy expects a JsonObject. We rebuild the manifest
    // in-memory here — it costs a few KB of transient heap (caller
    // pre-sized out_body) and there's no other way to expose the
    // schema-driven UI to Mothership without reaching for the wire.
    if (String(method) == "GET" && String(path) == UI_MANIFEST_PATH) {
      buildManifestObject(out_body.to<JsonObject>());
      out_status = 200;
      return true;
    }
    // Generic proxy-endpoint registry. Modules that mount their REST
    // handler directly on AsyncWebServer (rather than as an
    // IWebFeatureEntry — see e.g. FeaturesService, which only serves
    // a tiny JSON of build-time flags) register an in-process handler
    // here so the mothership rest.proxy path can reach them. Walked
    // BEFORE _entries so a module can override an entry's proxy
    // behaviour if needed.
    for (auto& pe : _proxyEndpoints) {
      if (pe.method != method) continue;
      if (pe.path != path)     continue;
      if (pe.handler(body, out_body, out_status)) {
        return true;
      }
    }
    for (auto& e : _entries) {
      if (!e) continue;
      if (e->proxyDispatch(method, path, body, out_status, out_body)) {
        return true;
      }
    }
    return false;
  }

  // Register a generic in-process handler reachable via the
  // rest.proxy mothership action. handler signature:
  //   bool(JsonVariant body, JsonVariant out, int& out_status)
  // Return true and fill out_body + out_status on a hit; false to
  // fall through (e.g. wrong method).
  using ProxyEndpointHandler =
      std::function<bool(JsonVariant body, JsonVariant out, int& out_status)>;
  void registerProxyEndpoint(const String& method,
                              const String& path,
                              ProxyEndpointHandler handler) {
    ProxyEndpoint pe;
    pe.method  = method;
    pe.path    = path;
    pe.handler = std::move(handler);
    _proxyEndpoints.push_back(std::move(pe));
  }

  // Single-shot (non-streaming) manifest builder for rest.proxy +
  // device-side manifest_rev hashing. Same data shape as
  // serveManifest's authenticated tier — features, modules, device,
  // buildFeatures — but written into a pre-allocated JsonObject
  // instead of streamed via AsyncResponseStream. The caller is
  // responsible for document sizing (typical ~8 KB covers the full
  // manifest of a normal-sized consumer project).
  //
  // Authentication is intentionally bypassed — the caller is either
  // (a) the mothership rest.proxy path which has already cleared the
  // mTLS gate on /api/v1/checkin, or (b) the device's MothershipService
  // computing a CRC of its own manifest to send as manifest_rev.
  // Local-browser UI manifest fetch still goes through serveManifest
  // (which honors JWT auth).
  void buildManifestObject(JsonObject root);

 private:
  void serveManifest(AsyncWebServerRequest* request);

  // Two entries of one kind under one id shadow each other's routes and
  // tabs (e.g. an external HW instance named like a consumer feature): log
  // it. Not a template, so registerEntry<T> stays small.
  void warnIfDuplicate(const IWebFeatureEntry& e) const;

  // Build the manifest ONCE per firmware build into two on-flash
  // files (anon + authed tiers). The two files are rewritten only
  // when the manifest signature changes — practically: only on the
  // first boot after a firmware update. Subsequent reboots open the
  // existing files and serve them straight from flash. This avoids
  // burning LittleFS write cycles on every boot of a long-running
  // device.
  //
  // Signature is a CRC32 over a stable summary of the registered
  // modules + entries + framework + project versions; stored in a
  // sidecar file .manifest.sig. Mismatch (or missing files) →
  // rebuild + rewrite + update sig. Match → return without touching
  // flash.
  //
  // The manifest endpoint then streams the appropriate file via
  // AsyncFileResponse — zero per-request heap, immune to the
  // bad_alloc / std::terminate cascade we hit when ManifestBuilder
  // fires under heap pressure. Called from begin() right after
  // _entries / _modules are registered; by then heap is at boot
  // peak (max_alloc ~70-90 KB) so the one-shot build fits.
  void writeManifestCacheToFs();
  static void addDeviceTz(JsonObject device);

  static constexpr const char* kManifestCacheAnonPath
      = "/config/.manifest.anon.json";
  static constexpr const char* kManifestCacheAuthedPath
      = "/config/.manifest.authed.json";
  static constexpr const char* kManifestCacheSigPath
      = "/config/.manifest.sig";

  // Compute a 32-bit signature over the current manifest CONTENT —
  // CRC32 of the authed-tier bytes, so any change to any emitted field
  // (not merely id / version) yields a different value. Used by
  // writeManifestCacheToFs() to decide whether the on-flash cache is
  // current. Reuses the caller's ManifestBuilder by reference — a second
  // ~4 KB builder on this boot stack path overflows the loopTask stack.
  uint32_t computeManifestSig(ManifestBuilder& builder) const;

  AsyncWebServer* _server{nullptr};
  SecurityManager* _sm{nullptr};
  WsManager* _wsManager{nullptr};
  const char* _deviceName{""};
  const char* _deviceVersion{""};

  std::vector<std::unique_ptr<IWebFeatureEntry>> _entries;
  ManifestArray _extraArray;

  struct ProxyEndpoint {
    String               method;
    String               path;
    ProxyEndpointHandler handler;
  };
  std::vector<ProxyEndpoint> _proxyEndpoints;


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

  // Content signature of the current manifest (computeManifestSig),
  // cached at boot by writeManifestCacheToFs(). Doubles as the ETag base
  // for UI-manifest conditional GETs — it changes only when the
  // manifest content changes (a firmware update), so the browser can
  // revalidate with a cheap 304 instead of re-streaming the ~15 KB body
  // on every page load. 0 = not yet computed → ETag path skipped.
  uint32_t _manifestSig{0};
  uint32_t _manifestBytes[2]{0, 0};
  void noteManifestBytes(bool authed, size_t bytes);
  bool _manifestLive{false};  // writes were paused at the last re-bake: build on the fly

  // Singleton manifest builder — BSS-resident, NOT allocated per
  // request. Mutex guards concurrent UI-manifest fetches; a
  // request that arrives while another is still streaming gets 503.
  // For our typical IoT workload (one operator at a time) the mutex
  // is essentially never contended; if a browser parallel-fetches
  // the manifest alongside other endpoints, the second fetch just
  // retries. Eliminates the per-request std::make_shared<>() that
  // used to throw bad_alloc under heap pressure and trigger a
  // device reboot via std::terminate.
  ManifestBuilder    _manifestBuilder;
  SemaphoreHandle_t  _manifestMutex{nullptr};
};

#endif
