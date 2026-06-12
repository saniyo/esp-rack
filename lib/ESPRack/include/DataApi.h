#ifndef DataApi_h
#define DataApi_h

// DataApi — phase C5 of docs/plans/api-standardization.md.
//
// The read-only surface an external server polls for data the device
// ACCUMULATES over time (sensor series, event logs) — distinct from
// current-state values, which live on each feature's /state face.
//
//   GET /api/v1/data                      → {"series":[{id,title,unit}...]}
//   GET /api/v1/data/<id>?limit&cursor    → {"series":id, items:[...], nextCursor?}
//   GET /api/v1/data/<id>/latest          → same shape, newest single item
//
// Modules register a series the way they register features:
//
//   DataApi::instance().registerSeries(
//       {"light.events", "Light events", ""},
//       [this](JsonObject& out, uint32_t cursor, size_t limit) { ... });
//
// Filler contract: write up to `limit` items NEWEST-FIRST into
// out["items"] starting at offset `cursor` (0 = newest), and set
// out["nextCursor"] (the next offset) when more remain. Pagination is
// MANDATORY by design — the no-PSRAM C3 targets ≤4 KB responses, so
// the limit is clamped to kMaxLimit and the response doc is fixed at
// kDocBytes; a filler that needs more per item simply returns fewer.
//
// Auth: Authenticated GET — a `reader` machine token is exactly enough
// (this API never mutates, so the operator floor does not apply).
// Mounted as ONE prefix handler at /api/v1/data: AsyncWebServer
// matches "<uri>/..." subpaths, which is exactly the dispatch we want
// here — and nothing else may ever mount under /api/v1/data/ (the
// endpoint-prefix rule from the C1 path table).

#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ProblemJson.h>
#include <SecurityManager.h>
#include <functional>

struct DataSeriesDef {
  const char* id;     // path segment — lowercase LDH + dots, stable
  const char* title;  // human label for the series list
  const char* unit;   // optional unit hint ("" when not applicable)
};

using DataSeriesFill =
    std::function<void(JsonObject& out, uint32_t cursor, size_t limit)>;

class DataApi {
 public:
  static constexpr size_t kMaxSeries = 16;
  static constexpr size_t kDocBytes  = 4096;
  static constexpr size_t kMaxLimit  = 100;

  static DataApi& instance() {
    static DataApi i;
    return i;
  }

  bool registerSeries(const DataSeriesDef& def, DataSeriesFill fill) {
    if (!def.id || !*def.id || !fill || _count >= kMaxSeries) return false;
    _series[_count].def  = def;
    _series[_count].fill = std::move(fill);
    ++_count;
    return true;
  }

  // Mount the prefix handler. Called once from App::begin AFTER modules
  // installed (so the SecurityManager is the real one) and BEFORE
  // server->begin(). With no SecurityManager the route stays open —
  // consistent with the FT_SECURITY=0 posture.
  void mount(AsyncWebServer* server, SecurityManager* sm) {
    if (!server || _mounted) return;
    auto handler = [this](AsyncWebServerRequest* r) { this->handle(r); };
    server->on("/api/v1/data", HTTP_GET,
               sm ? sm->wrapRequest(handler, AuthenticationPredicates::IS_AUTHENTICATED)
                  : ArRequestHandlerFunction(handler));
    _mounted = true;
  }

  size_t count() const { return _count; }

 private:
  struct Entry {
    DataSeriesDef  def{};
    DataSeriesFill fill;
  };

  void handle(AsyncWebServerRequest* r) {
    // Tolerance guard — same contract as every other JSON producer.
    if (ESP.getMaxAllocHeap() < kDocBytes + 1024) {
      sendProblem(r, 503, "heap_pressure",
                  "Data response needs more contiguous heap than is free", 1);
      return;
    }

    // Dispatch on the path tail past the mount prefix.
    String rest = r->url().substring(sizeof("/api/v1/data") - 1);
    bool latest = false;
    String id;
    if (rest.length() > 1) {
      id = rest.substring(1);  // strip leading '/'
      if (id.endsWith("/latest")) {
        latest = true;
        id.remove(id.length() - (sizeof("/latest") - 1));
      }
    }

    AsyncJsonResponse* resp = new AsyncJsonResponse(false, kDocBytes);
    JsonObject root = resp->getRoot().to<JsonObject>();
    if (root.isNull()) {
      delete resp;
      sendProblem(r, 503, "heap_pressure", "Response buffer alloc failed", 1);
      return;
    }

    if (id.isEmpty()) {
      // GET /api/v1/data — the series directory.
      JsonArray arr = root.createNestedArray("series");
      for (size_t i = 0; i < _count; ++i) {
        JsonObject s = arr.createNestedObject();
        s["id"]    = _series[i].def.id;
        s["title"] = _series[i].def.title ? _series[i].def.title : "";
        if (_series[i].def.unit && *_series[i].def.unit) {
          s["unit"] = _series[i].def.unit;
        }
      }
      resp->setLength();
      r->send(resp);
      return;
    }

    for (size_t i = 0; i < _count; ++i) {
      if (id != _series[i].def.id) continue;
      uint32_t cursor = 0;
      size_t   limit  = 50;
      if (!latest) {
        if (r->hasParam("cursor")) {
          cursor = (uint32_t)r->getParam("cursor")->value().toInt();
        }
        if (r->hasParam("limit")) {
          long l = r->getParam("limit")->value().toInt();
          if (l >= 1 && (size_t)l <= kMaxLimit) limit = (size_t)l;
        }
      } else {
        limit = 1;
      }
      root["series"] = _series[i].def.id;
      _series[i].fill(root, cursor, limit);
      if (resp->overflowed()) {
        // Filler outgrew the fixed doc — drop the response rather than
        // ship a silently truncated page; the caller halves `limit`.
        delete resp;
        sendProblem(r, 503, "page_too_large",
                    "Requested page does not fit the response budget - lower `limit`", 0);
        return;
      }
      resp->setLength();
      r->send(resp);
      return;
    }

    delete resp;
    sendProblem(r, 404, "series_not_found", "No data series with that id");
  }

  Entry  _series[kMaxSeries];
  size_t _count{0};
  bool   _mounted{false};
};

#endif  // DataApi_h
