#include <WebManager.h>
#include <ESPFS.h>
#include "power/PowerGuard.h"
#include <ProblemJson.h>
#include <esp_rom_crc.h>
#include <time/TimeSync.h>

// ── ManifestBuilder method definitions ─────────────────────────────
// The class itself is declared in WebManager.h so the singleton
// instance can live as a WebManager member (BSS, not per-request
// heap). Method bodies stay here to keep the header lean and
// recompiles cheap when state-machine logic changes.

void ManifestBuilder::reset(const WebManager& mgr, bool authed) {
  _mgr     = &mgr;
  _authed  = authed;
  _modIdx  = 0;
  _featIdx = 0;
  _extraIdx = 0;
  _cursor  = 0;
  _itemLen = 0;
  _doc.clear();
  advanceTo(Phase::HEADER);
}

size_t ManifestBuilder::fillNext(uint8_t* buf, size_t maxLen) {
  size_t written = 0;
  while (written < maxLen && _phase != Phase::DONE) {
    if (_cursor >= _itemLen) {
      advancePhase();
      if (_phase == Phase::DONE) break;
      continue;
    }
    size_t remaining = _itemLen - _cursor;
    size_t take = (remaining < (maxLen - written)) ? remaining : (maxLen - written);
    std::memcpy(buf + written,
                reinterpret_cast<const uint8_t*>(_itemBuf) + _cursor,
                take);
    _cursor += take;
    written += take;
  }
  return written;
}

void ManifestBuilder::setLiteral(const char* s) {
  size_t n = std::strlen(s);
  if (n >= sizeof(_itemBuf)) n = sizeof(_itemBuf) - 1;
  std::memcpy(_itemBuf, s, n);
  _itemBuf[n] = '\0';
  _itemLen = n;
  _cursor = 0;
}

void ManifestBuilder::renderDocIntoItem() {
  _itemLen = serializeJson(_doc, _itemBuf, sizeof(_itemBuf));
  _cursor = 0;
  _doc.clear();
}

void ManifestBuilder::advanceTo(Phase p) {
  _phase = p;
  _cursor = 0;
  _itemLen = 0;
  fillPhaseBuffer();
}

void ManifestBuilder::advancePhase() {
  switch (_phase) {
    case Phase::HEADER:             advanceTo(Phase::DEVICE_OPEN); return;
    case Phase::DEVICE_OPEN:        advanceTo(Phase::DEVICE); return;
    case Phase::DEVICE:             advanceTo(Phase::BUILDFEATURES_OPEN); return;
    case Phase::BUILDFEATURES_OPEN: advanceTo(Phase::BUILDFEATURES); return;
    case Phase::BUILDFEATURES:
      advanceTo(_authed ? Phase::AUTH_TRUE : Phase::AUTH_FALSE_END);
      return;
    case Phase::AUTH_FALSE_END:     advanceTo(Phase::DONE); return;
    case Phase::AUTH_TRUE:          advanceTo(Phase::MODULES_OPEN); return;
    case Phase::MODULES_OPEN:
      _modIdx = 0;
      if (_mgr->_modules.empty()) { advanceTo(Phase::MODULES_CLOSE); return; }
      advanceTo(Phase::MODULES_ITEM);
      return;
    case Phase::MODULES_ITEM:
      _modIdx++;
      if (_modIdx < _mgr->_modules.size()) {
        advanceTo(Phase::MODULES_SEP);
      } else {
        advanceTo(Phase::MODULES_CLOSE);
      }
      return;
    case Phase::MODULES_SEP:        advanceTo(Phase::MODULES_ITEM); return;
    case Phase::MODULES_CLOSE:      advanceTo(Phase::FEATURES_OPEN); return;
    case Phase::FEATURES_OPEN:
      _featIdx = 0;
      while (_featIdx < _mgr->_entries.size() && !_mgr->_entries[_featIdx]) {
        _featIdx++;
      }
      if (_featIdx >= _mgr->_entries.size()) {
        advanceTo(Phase::FEATURES_CLOSE);
      } else {
        advanceTo(Phase::FEATURES_ITEM);
      }
      return;
    case Phase::FEATURES_ITEM: {
      size_t next = _featIdx + 1;
      while (next < _mgr->_entries.size() && !_mgr->_entries[next]) next++;
      if (next < _mgr->_entries.size()) {
        _featIdx = next;
        advanceTo(Phase::FEATURES_SEP);
      } else {
        advanceTo(Phase::FEATURES_CLOSE);
      }
      return;
    }
    case Phase::FEATURES_SEP:       advanceTo(Phase::FEATURES_ITEM); return;
    case Phase::FEATURES_CLOSE: {
      const auto& x = _mgr->_extraArray;
      advanceTo(x.key && x.count && x.item ? Phase::EXTRA_OPEN : Phase::FOOTER);
      return;
    }
    case Phase::EXTRA_OPEN:
      _extraIdx = 0;
      advanceTo(_mgr->_extraArray.count() ? Phase::EXTRA_ITEM : Phase::EXTRA_CLOSE);
      return;
    case Phase::EXTRA_ITEM:
      if (++_extraIdx < _mgr->_extraArray.count()) {
        advanceTo(Phase::EXTRA_SEP);
      } else {
        advanceTo(Phase::EXTRA_CLOSE);
      }
      return;
    case Phase::EXTRA_SEP:          advanceTo(Phase::EXTRA_ITEM); return;
    case Phase::EXTRA_CLOSE:        advanceTo(Phase::FOOTER); return;
    case Phase::FOOTER:             advanceTo(Phase::DONE); return;
    case Phase::DONE:               return;
  }
}

void ManifestBuilder::fillPhaseBuffer() {
  switch (_phase) {
    case Phase::HEADER:              setLiteral("{\"schemaVersion\":3"); return;
    case Phase::DEVICE_OPEN:         setLiteral(",\"device\":"); return;
    case Phase::BUILDFEATURES_OPEN:  setLiteral(",\"buildFeatures\":"); return;
    case Phase::AUTH_FALSE_END:      setLiteral(",\"authenticated\":false}"); return;
    case Phase::AUTH_TRUE:           setLiteral(",\"authenticated\":true"); return;
    case Phase::MODULES_OPEN:        setLiteral(",\"modules\":["); return;
    case Phase::MODULES_SEP:         setLiteral(","); return;
    case Phase::MODULES_CLOSE:       setLiteral("]"); return;
    case Phase::FEATURES_OPEN:       setLiteral(",\"features\":["); return;
    case Phase::FEATURES_SEP:        setLiteral(","); return;
    case Phase::FEATURES_CLOSE:      setLiteral("]"); return;
    case Phase::EXTRA_SEP:           setLiteral(","); return;
    case Phase::EXTRA_CLOSE:         setLiteral("]"); return;
    case Phase::EXTRA_OPEN: {
      char lit[48];
      snprintf(lit, sizeof(lit), ",\"%s\":[", _mgr->_extraArray.key);
      setLiteral(lit);
      return;
    }
    case Phase::FOOTER:              setLiteral("}"); return;
    case Phase::DONE:                _itemLen = 0; _cursor = 0; return;

    case Phase::DEVICE: {
      JsonObject d = _doc.to<JsonObject>();
      d["name"]    = _mgr->_deviceName;
      d["version"] = _mgr->_deviceVersion;
      if (_mgr->_frameworkVersion && *_mgr->_frameworkVersion) {
        d["frameworkVersion"] = _mgr->_frameworkVersion;
      }
      WebManager::addDeviceTz(d);
      renderDocIntoItem();
      return;
    }
    case Phase::BUILDFEATURES: {
      JsonObject o = _doc.to<JsonObject>();
      for (const auto& b : _mgr->_buildFeatures) {
        if (b.key) o[b.key] = b.enabled;
      }
      renderDocIntoItem();
      return;
    }
    case Phase::MODULES_ITEM: {
      const auto& m = _mgr->_modules[_modIdx];
      JsonObject o = _doc.to<JsonObject>();
      o["id"]      = m.id;
      o["version"] = m.version;
      renderDocIntoItem();
      return;
    }
    case Phase::FEATURES_ITEM: {
      JsonObject o = _doc.to<JsonObject>();
      _mgr->_entries[_featIdx]->toJson(o);
      renderDocIntoItem();
      return;
    }
    case Phase::EXTRA_ITEM: {
      JsonObject o = _doc.to<JsonObject>();
      _mgr->_extraArray.item(_extraIdx, o);
      renderDocIntoItem();
      return;
    }
  }
}

WebManager::WebManager(AsyncWebServer* server,
                       SecurityManager* sm,
                       WsManager* wsManager,
                       const char* deviceName,
                       const char* deviceVersion)
    : _server(server),
      _sm(sm),
      _wsManager(wsManager),
      _deviceName(deviceName ? deviceName : ""),
      _deviceVersion(deviceVersion ? deviceVersion : "") {}

void WebManager::forEachHwPage(const char* hwId, const std::function<void(const char*)>& fn) const {
  if (!hwId) return;
  for (const auto& e : _entries) {
    const char* hw = e ? e->menuHw() : nullptr;
    if (hw && strcmp(hw, hwId) == 0) fn(e->id());
  }
}

void WebManager::warnIfDuplicate(const IWebFeatureEntry& n) const {
  const char* id = n.id();
  if (!id || !*id) return;
  for (const auto& e : _entries) {
    if (e->kind() == n.kind() && strcmp(e->id(), id) == 0) {
      log_e("[web] duplicate entry id '%s'", id);
      return;
    }
  }
}

void WebManager::begin() {
  if (_begun || !_server) return;

  // Create the manifest-builder serialisation mutex on first begin().
  // FreeRTOS mutex is ~80 bytes one-time heap; created at boot when
  // heap is uncontested. Lives for the lifetime of the program.
  if (!_manifestMutex) {
    _manifestMutex = xSemaphoreCreateMutex();
  }

  for (auto& e : _entries) {
    if (e) e->registerEndpoints(_server, _sm);
  }

  // Manifest endpoint is two-tier (auth-aware) — it ALWAYS responds, but
  // the payload depends on whether the request carries a valid JWT:
  //   * Anonymous → minimal stub: schemaVersion + device.{name,version,
  //     frameworkVersion} + buildFeatures.security. Enough for the
  //     SignIn page to render the right brand and decide whether to show
  //     a login form, no leak of the endpoint inventory.
  //   * Authenticated → full manifest: features[], modules[], all
  //     buildFeatures, action endpoints, etc. The whole map.
  // Per-entry endpoints continue to enforce their own predicates; the
  // manifest just controls how much *map* is exposed to anonymous
  // observers. ManifestLoader on the frontend re-fetches after sign-in
  // to swap stub for full content (see ManifestContext.reload()).
  ArRequestHandlerFunction handler =
      [this](AsyncWebServerRequest* request) { this->serveManifest(request); };
  _server->on(UI_MANIFEST_PATH, HTTP_GET, handler);

  // Pre-bake the manifest to LittleFS so the UI-manifest endpoint is a pure
  // file stream from this point on. Signature gate inside ensures we
  // only rewrite on firmware changes — no flash wear on every boot.
  // Heap is at boot peak here (~70-90 KB max_alloc), so the one-shot
  // ManifestBuilder cost is comfortable.
  writeManifestCacheToFs();

  _begun = true;
}

// Streamed manifest assembly. Earlier revisions buffered the whole
// payload into a single AsyncJsonResponse — fine at 2-3 modules,
// catastrophic past ~12 features because the fixed buffer silently
// truncated mid-stream and dropped trailing entries (which manifested
// as "menu items mysteriously missing"). Now we beginResponseStream()
// and serialize each entry into its own small per-entry document, so
// total manifest size is bounded only by the network pipe — adding
// modules costs zero buffer pressure.
//
// Per-entry doc is sized for the largest single feature spec we expect
// (full WebFeatureSpec with tabs + actions). 4 KB leaves comfortable
// headroom; if a single entry ever exceeded this we'd see truncation
// of THAT entry only, never of unrelated entries downstream.
// Single-shot manifest builder for the rest.proxy path. Browser-facing
// UI-manifest fetch stays on AsyncResponseStream — this is the in-process
// alternative that fills a pre-allocated JsonObject so Mothership can
// snapshot the device's UI schema without an HTTPS round trip on each
// poll.
// device.tz = {iana, posix} (SES-LIB-015): the UI formats device-local
// times with it. Omitted while no timezone is set.
void WebManager::addDeviceTz(JsonObject device) {
  char posix[64], iana[64];
  ESPRack::TimeSync::instance().getTimezone(posix, sizeof(posix), iana, sizeof(iana));
  if (!posix[0] && !iana[0]) return;
  JsonObject tz = device.createNestedObject("tz");
  if (iana[0]) tz["iana"] = (char*)iana;     // copied
  if (posix[0]) tz["posix"] = (char*)posix;  // copied
}

void WebManager::refreshManifestCache() {
  if (_manifestSig == 0) return;  // begin() bakes it
  writeManifestCacheToFs();
}

void WebManager::buildManifestObject(JsonObject root) {
  root["schemaVersion"] = 3;

  JsonObject device = root.createNestedObject("device");
  device["name"]    = _deviceName;
  device["version"] = _deviceVersion;
  if (_frameworkVersion && *_frameworkVersion) {
    device["frameworkVersion"] = _frameworkVersion;
  }
  addDeviceTz(device);

  JsonObject bf = root.createNestedObject("buildFeatures");
  for (const auto& b : _buildFeatures) {
    if (b.key) bf[b.key] = b.enabled;
  }

  // Trust boundary is the caller's mTLS handshake — always emit full
  // authenticated tier.
  root["authenticated"] = true;

  JsonArray modules = root.createNestedArray("modules");
  for (const auto& m : _modules) {
    JsonObject mo = modules.createNestedObject();
    mo["id"]      = m.id;
    mo["version"] = m.version;
  }

  JsonArray features = root.createNestedArray("features");
  for (const auto& e : _entries) {
    if (!e) continue;
    JsonObject fo = features.createNestedObject();
    e->toJson(fo);
  }

  if (_extraArray.key && _extraArray.count && _extraArray.item) {
    JsonArray xa = root.createNestedArray(_extraArray.key);
    const size_t n = _extraArray.count();
    for (size_t i = 0; i < n; ++i) _extraArray.item(i, xa.createNestedObject());
  }
}

// Pump the ManifestBuilder into a LittleFS file. Used by
// writeManifestCacheToFs() once per tier at boot. Lives at file
// scope so the routine reads top-down. Returns true if the full
// blob landed on flash without write errors.
static bool dumpBuilderToFile(ManifestBuilder& b, fs::File& f) {
  uint8_t buf[256];
  while (true) {
    size_t n = b.fillNext(buf, sizeof(buf));
    if (n == 0) return true;            // builder done
    size_t w = f.write(buf, n);
    if (w != n) {
      log_e("[web.manifest] short write to flash (%u/%u)",
            (unsigned)w, (unsigned)n);
      return false;
    }
  }
}

uint32_t WebManager::computeManifestSig(ManifestBuilder& builder) const {
  // CRC32 over the ACTUAL authed-tier manifest bytes — the exact stream the
  // flash cache stores. The authed tier is a superset of the anon stub
  // (device + buildFeatures + modules + features), so folding it covers
  // every emitted field: menu.parent / menu.expandTabs, tab keys, rest
  // paths, route, component, version, order — not merely the entry id.
  //
  // Supersedes the former id-only fold, which silently kept the flash cache
  // STALE whenever an entry changed its spec body but kept its id — e.g.
  // adding tree-menu nesting (menu.parent / expandTabs) bumped no id and no
  // version, so the cache was never rebuilt and the device kept serving the
  // pre-change manifest. That hole is now closed structurally; no reliance
  // on the consumer remembering to bump FACTORY_PROJECT_VERSION.
  //
  // Stack discipline: REUSES the caller's builder (by ref) — it does NOT
  // construct its own. writeManifestCacheToFs reserves ~4 KB of stack for its
  // `builder` local across the whole frame; a second builder here would
  // coexist with it and, layered under ArduinoJson's recursive serialization,
  // overflow the 8 KB loopTask stack (observed: "Stack canary watchpoint
  // triggered" at boot). One shared builder keeps peak stack identical to the
  // original id-only path. No heap, no flash write here.
  builder.reset(*this, /*authed=*/true);
  // Cache-signature salt — fold a generation tag into the CRC ahead of the
  // manifest bytes so a framework-level path/schema migration forces every
  // device to rebuild its flash cache on the first boot after the update,
  // even in the (impossible-in-practice) event the new content CRCs to the
  // same value as the old. Bump on any cross-cutting manifest format change.
  // 0x0A_01_00_03 = api-v1 (C1), manifest schemaVersion 3.
  static constexpr uint32_t kManifestSigSalt = 0x0A010003u;
  uint32_t crc = 0xFFFFFFFF;
  crc = esp_rom_crc32_le(crc, reinterpret_cast<const uint8_t*>(&kManifestSigSalt),
                         sizeof(kManifestSigSalt));
  uint8_t buf[256];
  for (size_t n = builder.fillNext(buf, sizeof(buf)); n;
       n = builder.fillNext(buf, sizeof(buf))) {
    crc = esp_rom_crc32_le(crc, buf, n);
  }
  return crc;
}

void WebManager::writeManifestCacheToFs() {
  // Build anon + authed manifests into two flash files — but only
  // when the signature differs from what's currently on flash. Each
  // build burns ~30 KB of LittleFS write traffic; doing it every boot
  // on a long-running device that reboots a few times a week would
  // wear out a SPI flash sector inside a year. With the signature
  // gate, writes happen at most once per firmware update.
  //
  // Best-effort on FS errors: log and let serveManifest fall back to
  // the legacy chunked-build path. Soft-degrade rather than refusing
  // to boot.
  // ONE ManifestBuilder for this whole routine: the signature walk
  // (computeManifestSig) and the anon+authed cache writes all reuse it.
  // See computeManifestSig for why a second builder would blow the stack.
  ManifestBuilder builder;
  const uint32_t sig = computeManifestSig(builder);
  // Cache the signature for serveManifest()'s ETag (conditional-GET
  // revalidation). Set unconditionally — both the cache-hit early-return
  // and the rebuild path below need it live for the process lifetime.
  _manifestSig = sig;

  // Read sidecar signature file. If it matches AND both manifest
  // files exist, we're already current — skip the rewrite.
  bool cache_ok = false;
  {
    fs::File sf = ESPFS.open(kManifestCacheSigPath, "r");
    if (sf && sf.size() == sizeof(sig)) {
      uint32_t stored = 0;
      sf.read(reinterpret_cast<uint8_t*>(&stored), sizeof(stored));
      cache_ok = (stored == sig)
              && ESPFS.exists(kManifestCacheAnonPath)
              && ESPFS.exists(kManifestCacheAuthedPath);
      sf.close();
    }
  }
  if (cache_ok) {
    log_i("[web.manifest] cache sig 0x%08x matches — reusing existing flash files",
          (unsigned)sig);
    _manifestLive = false;
    return;
  }
  // PowerGuard (SES-LIB-008): no flash writes now. Serve the manifest built
  // on the fly until the next re-bake, never the stale files.
  if (!ESPRack::PowerGuard::writesAllowed()) {
    _manifestLive = true;
    log_w("[web.manifest] writes paused: serving the manifest live");
    return;
  }
  _manifestLive = false;

  log_i("[web.manifest] cache sig changed (or missing) — rebuilding to flash, sig=0x%08x",
        (unsigned)sig);

  // Each tier goes to "<path>.tmp" first and replaces the file only when
  // complete: a runtime re-bake (refreshManifestCache) must never let a
  // concurrent GET stream a half-written file. Between remove and rename
  // serveManifest finds no file and builds on the fly.
  for (int authed = 0; authed < 2; ++authed) {
    const char* path = authed ? kManifestCacheAuthedPath : kManifestCacheAnonPath;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fs::File f = ESPFS.open(tmp, "w");
    if (!f) {
      log_w("[web.manifest] cannot open %s for write", tmp);
      continue;
    }
    builder.reset(*this, authed != 0);
    const bool ok = dumpBuilderToFile(builder, f);
    const size_t sz = f.size();
    f.close();
    if (!ok) {
      log_w("[web.manifest] %s cache TRUNCATED (%u B), keeping the old one",
            authed ? "authed" : "anon", (unsigned)sz);
      ESPFS.remove(tmp);
      continue;
    }
    if (ESPFS.exists(path)) ESPFS.remove(path);
    if (!ESPFS.rename(tmp, path)) {
      log_w("[web.manifest] rename %s failed", tmp);
      continue;
    }
    log_i("[web.manifest] wrote %s cache: %u B", authed ? "authed" : "anon", (unsigned)sz);
  }
  // Commit the new signature LAST. If a reboot interrupts the rewrite
  // mid-flight, the old (mismatching) signature stays — next boot
  // notices the mismatch and retries. Sig-file written last keeps the
  // detection honest.
  {
    fs::File sf = ESPFS.open(kManifestCacheSigPath, "w");
    if (!sf) {
      log_w("[web.manifest] cannot open sig file for write");
    } else {
      sf.write(reinterpret_cast<const uint8_t*>(&sig), sizeof(sig));
      sf.close();
    }
  }
}

void WebManager::serveManifest(AsyncWebServerRequest* request) {
  // Auth probe — ALWAYS replies, never 401s here. The result decides
  // payload depth: anonymous = stub; authenticated = full. Real
  // endpoints behind the manifest still 401 on their own.
  bool authed = true;
  if (_sm) {
    Authentication auth = _sm->authenticateRequest(request);
    authed = auth.authenticated;
  }
  const char* path = authed ? kManifestCacheAuthedPath
                            : kManifestCacheAnonPath;

  // Conditional-GET revalidation. The manifest body changes ONLY on a
  // firmware update (computeManifestSig is a content hash cached in
  // _manifestSig at boot). Tag every response "<sig>-<a|n>" — auth state
  // is baked in so an anon browser can never revalidate into the authed
  // manifest (or vice-versa). The browser caches the ~15 KB body once and
  // sends a conditional GET on later loads; we answer 304 (a few bytes)
  // instead of re-streaming 15 KB. On the no-PSRAM C3 that re-stream is
  // the single biggest concurrent fetch that craters maxAlloc and
  // collapses the menu to "only Security" — the 304 sidesteps it.
  // Worst case (browser omits/garbles If-None-Match) = a normal 200, no
  // regression; a stale-serve is impossible because a new firmware sig
  // yields a new ETag that the old If-None-Match can never match.
  String etag;
  if (_manifestSig != 0) {
    etag = "\"";
    etag += String(_manifestSig, HEX);
    etag += authed ? "-a\"" : "-n\"";
    if (request->hasHeader("If-None-Match")
        && request->header("If-None-Match") == etag) {
      AsyncWebServerResponse* nm = request->beginResponse(304, "text/plain", "");
      nm->addHeader("ETag", etag);
      nm->addHeader("Cache-Control", "private, no-cache");
      request->send(nm);
      return;
    }
  }

  // Happy path: serve directly from LittleFS via AsyncFileResponse.
  // AsyncWebServer streams the file in PBUF_POOL-sized chunks straight
  // into the TCP write loop — no heap-resident copy of the payload,
  // no per-request ManifestBuilder, no std::make_shared. Whatever the
  // current heap pressure, this path is allocation-free apart from
  // the AsyncBasicResponse object itself (~150 B).
  if (!_manifestLive && ESPFS.exists(path)) {
    AsyncWebServerResponse* res = request->beginResponse(ESPFS, path,
                                                         "application/json");
    // Tell the browser not to keep this socket open after the response.
    // The manifest is fetched once at page load; sustained keepalive
    // ties up an lwIP tcp_pcb slot we'd rather hand to the asset
    // requests that follow. Cuts the chance that the device runs out
    // of TCP state while the page is finishing its initial cascade.
    res->addHeader("Connection", "close");
    if (etag.length()) {
      // Cacheable but must-revalidate: the browser stores the body and
      // re-checks via If-None-Match → the 304 fast-path above on a match.
      res->addHeader("ETag", etag);
      res->addHeader("Cache-Control", "private, no-cache");
    } else {
      res->addHeader("Cache-Control", "no-store");
    }
    request->send(res);
    return;
  }

  // Fallback: cache file missing (FS write failed at boot, or got
  // deleted out from under us). Rebuild on the fly via ManifestBuilder
  // + chunked response. Same OOM guard the old path used.
  if (ESP.getMaxAllocHeap() < 4096) {
    log_w("[web.manifest] cache missing AND heap pressure (max_alloc=%u) — 503",
          (unsigned)ESP.getMaxAllocHeap());
    sendProblem(request, 503, "heap_pressure",
                "Manifest cache missing and heap too tight to rebuild", 2);
    return;
  }
  log_w("[web.manifest] cache file missing — falling back to on-the-fly build");
  auto builder = std::make_shared<ManifestBuilder>();
  builder->reset(*this, authed);
  AsyncWebServerResponse* response = request->beginChunkedResponse(
      "application/json",
      [builder](uint8_t* buf, size_t maxLen, size_t /*index*/) -> size_t {
        return builder->fillNext(buf, maxLen);
      });
  request->send(response);
}
