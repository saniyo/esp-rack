#include <WebEndpointsService.h>

#include <AdaptiveJson.h>
#include <FormBuilder.h>
#include <WebManager.h>
#include <WsManager.h>
#include <memory>
#include <cstring>

WebEndpointsService::WebEndpointsService(AsyncWebServer* server,
                                         SecurityManager* securityManager,
                                         WebManager* webManager,
                                         WsManager* wsManager)
    : _server(server), _sm(securityManager), _wm(webManager), _ws(wsManager) {
  // Route binding moved into registerManifest (WebPageSpec) so the
  // endpoint flows through WebManager like every other bespoke page.
  (void)_server; (void)_sm;
}

void WebEndpointsService::registerManifest(WebManager* web) {
  if (!web) return;
  using namespace std::placeholders;

  // Register the form-read endpoint via WebManager so rest.proxy can
  // reach WEB_ENDPOINTS_FORM_PATH (/api/v1/system/endpoints/form)
  // without HTTP loopback.
  WebPageSpec page;
  page.id        = "webEndpoints";
  page.title     = "Endpoints";
  page.component = "";   // no top-level page route; contributes a tab below
  page.auth      = WebAuthLevel::Admin;
  {
    WebEndpointMeta e;
    e.method  = "GET"; e.path = WEB_ENDPOINTS_FORM_PATH; e.role = "form";
    e.auth    = WebAuthLevel::Admin;
    e.handler = std::bind(&WebEndpointsService::serveForm, this, _1);
    e.internalHandler =
      [this](JsonVariant /*in*/, JsonVariant out, int& status) {
        JsonObject o = out.to<JsonObject>();
        buildForm(o);
        status = 200;
      };
    page.endpoints.push_back(std::move(e));
  }
  web->registerPage(std::move(page));

  WebTabSpec tab;
  tab.key      = "endpoints";
  tab.title    = "Endpoints";
  tab.restPath = WEB_ENDPOINTS_FORM_PATH;
  tab.postable = false;
  tab.live     = false;
  tab.auth     = WebAuthLevel::Admin;
  tab.order    = 40;
  web->addTabToFeature("system", tab);
}

// ===========================================================================
// File-local helpers + the chunked registry streamer. Defined BEFORE the
// member functions that use them (the chunked-response lambda needs the
// generator as a complete type at the capture site).
// ===========================================================================
namespace {

// Pull a field out of an entry's toJson() snapshot, returning empty
// String if missing. Wraps the JsonObject lookup so the body stays readable.
String j(JsonObjectConst src, const char* key) {
  JsonVariantConst v = src[key];
  if (!v.is<const char*>() && !v.is<String>()) return String();
  return v.as<String>();
}

// Same for nested rest{read,update}: the two typed-feature endpoints live
// under a sub-object; actions and the rest expose them flat.
String jNested(JsonObjectConst src, const char* objKey, const char* leafKey) {
  JsonObjectConst inner = src[objKey].as<JsonObjectConst>();
  if (inner.isNull()) return String();
  JsonVariantConst v = inner[leafKey];
  if (!v.is<const char*>() && !v.is<String>()) return String();
  return v.as<String>();
}

// Map one registry entry's toJson() snapshot (`src`) into a table `row`.
// Shared by the chunked streamer (HTTP) and buildForm (rest.proxy) so the
// column mapping can never diverge between the two paths.
void fillEntryRow(JsonObjectConst src, JsonObject& row) {
  row["id"] = j(src, "id");
  // String, NOT a temporary's c_str(): ArduinoJson stores const char* BY
  // POINTER, and a freed-String pointer rendered as garbage on the wire.
  String kindStr = j(src, "kind");
  if (kindStr.isEmpty()) kindStr = "?";
  row["kind"] = kindStr;
  String read = jNested(src, "rest", "read");
  if (read.length() == 0) read = j(src, "restPath");
  row["read"]   = read;
  row["write"]  = jNested(src, "rest", "update");
  row["ws"]     = j(src, "ws");
  row["method"] = j(src, "method");
  row["auth"]   = j(src, "auth");
}

// Add the entries registry table COLUMN definition (no rows) to `ep`.
// Single source of truth for the column set, shared by buildForm and the
// streamer's tail/skeleton so headers can never drift apart.
JsonObject addEntriesTable(JsonArray& ep) {
  return FormBuilder::addTableField(ep, "entries", AF::R,
      col("id",     "ID",       "text"),
      col("kind",   "Kind",     "text"),
      col("read",   "REST GET", "text"),
      col("write",  "REST POST","text"),
      col("ws",     "WS",       "text"),
      col("method", "Method",   "text"),
      col("auth",   "Auth",     "text"),
      icon("ListAlt"));
}

// Build EVERYTHING in the Endpoints form EXCEPT the entries-table rows:
// the summary/breakdown lines and the WS aggregate-health section (moved
// here from the old "Sockets" tab). Shared by buildForm (rest.proxy, which
// then appends the table inline) and the streamer (which serializes this
// once as the form "head", then streams the big table row by row).
void buildEndpointsHeader(JsonArray& ep, WebManager* wm, WsManager* ws) {
  // Pre-walk: totals + per-kind breakdown.
  size_t total = 0, cFeature = 0, cAction = 0, cPage = 0, cStatus = 0;
  wm->forEachEntry([&](const IWebFeatureEntry& e) {
    total++;
    switch (e.kind()) {
      case IWebFeatureEntry::Kind::Feature: cFeature++; break;
      case IWebFeatureEntry::Kind::Action:  cAction++;  break;
      case IWebFeatureEntry::Kind::Page:    cPage++;    break;
      case IWebFeatureEntry::Kind::Status:  cStatus++;  break;
    }
  });

  String totalStr = String(total) + " entries registered";
  FormBuilder::addTextField(ep, "total", AF::R, totalStr.c_str(), icon("Apps"));

  String mix = String("features: ") + cFeature + ", actions: " + cAction +
               ", pages: " + cPage + ", info: " + cStatus;
  FormBuilder::addTextField(ep, "breakdown", AF::R, mix.c_str(), icon("Dashboard"));

  if (!ws) return;

  // WS aggregate health. Per-client stats live on the Clients tab; here we
  // surface only roll-ups + the per-(endpoint,client) roster, which is
  // intrinsically endpoint-keyed. Heap doc (not stack): a big doc on the
  // async_tcp task stack overflowed it. Bounded roster → 2 KB suffices.
  DynamicJsonDocument stats(2048);
  if (stats.capacity() == 0) return;   // out of heap — skip WS section, stay valid
  JsonObject statsRoot = stats.to<JsonObject>();
  ws->fillStats(statsRoot);

  const uint32_t totalClients   = statsRoot["total_clients"] | 0;
  const float    avgRtt         = statsRoot["avg_rtt_ms"]    | 0.0f;
  const uint32_t pingIvl        = statsRoot["ping_interval_s"] | 0;
  const uint32_t pongTo         = statsRoot["pong_timeout_ms"] | 0;
  const uint32_t totalDrops     = statsRoot["total_dropped"] | 0;
  const uint32_t totalBcstDrops = statsRoot["total_broadcasts_dropped"] | 0;
  const uint32_t totalOverflow  = statsRoot["total_overflow_frames"] | 0;
  const uint32_t thr            = statsRoot["backpressure_threshold"]   | 0;
  const uint32_t maxQ           = statsRoot["backpressure_max"]         | 0;

  String wsClientStr = String(totalClients) + " across " +
                       String(statsRoot["endpoints"].size()) + " endpoints";
  FormBuilder::addTextField(ep, "ws_clients", AF::R, wsClientStr.c_str(),
                            icon("Computer"), color(totalClients > 0 ? "success" : "info"));

  String rttStr = String(avgRtt, 1) + " ms";
  FormBuilder::addTextField(ep, "ws_avg_rtt", AF::R, rttStr.c_str(), icon("NetworkPing"));

  String keepaliveStr = String("ping every ") + String(pingIvl) + " s, " +
                        "evict after " + String(pongTo / 1000UL) + " s silence";
  FormBuilder::addTextField(ep, "ws_keepalive", AF::R, keepaliveStr.c_str(), icon("Sync"));

  String bpStr = String(totalDrops) + " per-client / " + String(totalBcstDrops) +
                 " whole-broadcast / " + String(totalOverflow) + " oversized, threshold " +
                 String(thr) + "/" + String(maxQ);
  FormBuilder::addTextField(ep, "ws_backpressure", AF::R, bpStr.c_str(), icon("Warning"),
                            color((totalDrops || totalBcstDrops || totalOverflow) ? "warning" : "info"));

  JsonObject clTbl = FormBuilder::addTableField(ep, "ws_clients_table", AF::R,
      col("path",      "Endpoint",   "text"),
      col("id",        "ID",         "number", format("0")),
      col("ip",        "Remote IP",  "text"),
      col("age_s",     "Age (s)",    "number", format("0")),
      col("last_rx_s", "Rx age (s)", "number", format("0")),
      col("last_tx_s", "Tx age (s)", "number", format("0")),
      col("drops",     "Drops",      "number", format("0")),
      icon("Storage"));

  JsonArray clRows = clTbl["ws_clients_table"].as<JsonArray>();
  JsonArray eps    = statsRoot["endpoints"].as<JsonArray>();
  for (JsonObject epObj : eps) {
    const char* path = epObj["path"] | "";
    JsonArray cl = epObj["clients"].as<JsonArray>();
    for (JsonObject c : cl) {
      JsonObject row = clRows.createNestedObject();
      row["path"]      = path;
      row["id"]        = c["id"]        | 0;
      row["ip"]        = c["ip"]        | "";
      row["age_s"]     = c["age_s"]     | 0;
      row["last_rx_s"] = c["last_rx_s"] | 0;
      row["last_tx_s"] = c["last_tx_s"] | 0;
      row["drops"]     = c["drops"]     | 0;
    }
  }
}

// EndpointsChunker — TRUE incremental streaming of the Endpoints form over
// beginChunkedResponse. The async send loop pulls fill() repeatedly; each
// pull copies up to maxLen bytes out of a tiny rolling "stage" string. When
// the stage drains, we produce the NEXT piece:
//   head  — the summary + WS section, serialized ONCE (small, ~2 KB) and
//            degrade-safe (if the head doc can't allocate, we stream an
//            entries-only envelope instead of failing).
//   rows  — ONE registry entry serialized per refill, via the resumable
//            entryAt(i) accessor — the ~40-row table NEVER exists as one
//            contiguous tree, so it streams out under any heap pressure and
//            never sheds a 503 on size.
//   tail  — entries-table options + the closing brackets.
// Peak RAM during streaming ≈ head string + one entry doc (~2 KB) — no big
// contiguous block, which is the whole point. AsyncResponseStream was the
// wrong primitive: its cbuf grows (resizeAdd) to hold the WHOLE response,
// so it buffers rather than streams. beginChunkedResponse pulls on demand.
class EndpointsChunker {
 public:
  EndpointsChunker(WebManager* wm, WsManager* ws) : _wm(wm), _ws(ws) {
    buildHeadTail();
  }

  // Fill up to maxLen bytes; return count written. 0 == response complete.
  size_t fill(uint8_t* buf, size_t maxLen) {
    size_t w = 0;
    while (w < maxLen && !_done) {
      if (_pos >= _stage.length()) {
        // advance() resets _pos to 0 on EVERY call, including the terminal
        // phase-2->3 transition that returns false. Without _done, the next
        // fill() would see _pos(0) < _stage.length() (the tail still in
        // _stage) and re-emit the tail forever. _done latches completion so
        // termination can't be undone by that _pos reset.
        if (!advance()) { _done = true; break; }
        if (_stage.length() == 0) continue;
      }
      size_t n = _stage.length() - _pos;
      if (n > maxLen - w) n = maxLen - w;
      memcpy(buf + w, _stage.c_str() + _pos, n);
      w   += n;
      _pos += n;
      // Safety backstop: the real form is ~6 KB. If we ever blow past a
      // generous ceiling, a generator bug is running away — force-stop so a
      // runaway stream can NEVER again pin the single async_tcp task / heap
      // and take the whole device down. Truncates (invalid JSON) as a last
      // resort, which the UI surfaces as one failed tab — vastly better than
      // a device-wide outage. Must never fire in normal operation.
      _emitted += n;
      if (_emitted > kMaxStream) { _done = true; break; }
    }
    return w;
  }

 private:
  WebManager* _wm;
  WsManager*  _ws;
  String _head, _tail, _stage;
  size_t _pos      = 0;
  int    _phase    = 0;     // 0 head, 1 rows, 2 tail emitted, 3 done
  size_t _idx      = 0;     // registry cursor (resumable across refills)
  bool   _firstRow = true;
  bool   _done     = false; // latched once advance() signals completion
  size_t _emitted  = 0;     // total bytes streamed (runaway backstop)
  static constexpr size_t kMaxStream = 65536;  // ~10x the real ~6 KB form
  DynamicJsonDocument _entryDoc{2048};   // entry toJson() scratch, reused
  DynamicJsonDocument _rowDoc{512};      // mapped-row scratch, reused

  // Advance to the next stage of output. Returns false when fully done.
  bool advance() {
    _pos = 0;
    if (_phase == 0) { _stage = _head; _phase = 1; return true; }
    if (_phase == 1) {
      while (_idx < _wm->entryCount()) {
        const IWebFeatureEntry* e = _wm->entryAt(_idx++);
        if (!e) continue;
        _entryDoc.clear();
        JsonObject obj = _entryDoc.to<JsonObject>();
        e->toJson(obj);
        _rowDoc.clear();
        JsonObject row = _rowDoc.to<JsonObject>();
        fillEntryRow(obj, row);
        String s;
        serializeJson(row, s);
        _stage = _firstRow ? s : (String(',') + s);
        _firstRow = false;
        return true;
      }
      _stage = _tail; _phase = 2; return true;       // rows exhausted -> tail
    }
    if (_phase == 2) { _phase = 3; }
    return false;
  }

  // Build the head (envelope + summary + WS section, opening the entries
  // field) and the tail (entries-table options + closers) once at ctor.
  void buildHeadTail() {
    String body;
    {
      DynamicJsonDocument d(3072);   // freed at end of scope; degrade-safe
      if (d.capacity() > 0) {
        JsonObject root  = d.to<JsonObject>();
        JsonArray fields = FormBuilder::createForm(root, "endpoints", "Endpoints");
        buildEndpointsHeader(fields, _wm, _ws);
        serializeJson(d, body);
      }
    }
    // body == {"endpoints":{"description":"Endpoints","fields":[ <head fields> ]}}
    // Split off the trailing "]}}" and open the entries field in its place.
    const int close = body.lastIndexOf("]}}");
    if (close < 0) {
      // head doc couldn't allocate — stream an entries-only envelope.
      _head = F("{\"endpoints\":{\"description\":\"Endpoints\",\"fields\":[{\"entries\":[");
    } else {
      String prefix = body.substring(0, close);
      _head = prefix.endsWith("[") ? (prefix + F("{\"entries\":["))
                                   : (prefix + F(",{\"entries\":["));
    }

    // tail: take the entries-table skeleton's "],\"o\":\"...\"}" suffix, then
    // close the fields array + the two wrapping objects.
    String suffix = F("],\"o\":\"table;r\"}");   // fallback if skeleton fails
    {
      DynamicJsonDocument t(512);
      if (t.capacity() > 0) {
        JsonArray ta = t.to<JsonArray>();
        addEntriesTable(ta);
        String skel;
        serializeJson(ta[0], skel);              // {"entries":[],"o":"table;..."}
        const int br = skel.indexOf("[]");
        if (br >= 0) suffix = skel.substring(br + 1);   // ],"o":"..."}
      }
    }
    _tail = suffix + F("]}}");
  }
};

}  // namespace

void WebEndpointsService::serveForm(AsyncWebServerRequest* request) {
  // TRUE streaming: the registry table (~40 rows, ~9 KB as one tree) is
  // emitted one entry per pull via beginChunkedResponse, so no contiguous
  // block that big is ever needed and the guard below is effectively
  // unreachable on size — only a hard out-of-heap (can't even open the
  // ~1.5 KB stream + a 2 KB row scratch) degrades, and that degrades to an
  // entries-only envelope rather than a 503. buildForm stays for the
  // rest.proxy in-process path, where the mothership caller sizes the doc.
  if (!_wm) {
    sendProblem(request, 503, "unavailable", "WebManager not wired up", 5);
    return;
  }

  auto gen = std::make_shared<EndpointsChunker>(_wm, _ws);
  AsyncWebServerResponse* resp = request->beginChunkedResponse(
      "application/json",
      [gen](uint8_t* buf, size_t maxLen, size_t /*index*/) -> size_t {
        return gen->fill(buf, maxLen);
      });
  request->send(resp);
}

void WebEndpointsService::buildForm(JsonObject& root) {
  JsonArray ep = FormBuilder::createForm(root, "endpoints", "Endpoints");

  if (!_wm) {
    FormBuilder::addMessageField(ep, "err",
        "WebManager is not wired up.",
        level("error"), icon("Warning"));
    return;
  }

  // Summary + WS section (shared with the streamer so the two paths can't
  // diverge), then the registry table inline.
  buildEndpointsHeader(ep, _wm, _ws);

  JsonObject tbl  = addEntriesTable(ep);
  JsonArray  rows = tbl["entries"].as<JsonArray>();
  // ONE scratch doc reused across all entries (clear() resets the pool).
  // Row mapping shared with the streamer via fillEntryRow. This single-doc
  // builder is the rest.proxy path only; the HTTP GET path streams.
  DynamicJsonDocument tmp(2048);
  _wm->forEachEntry([&](const IWebFeatureEntry& e) {
    tmp.clear();
    JsonObject obj = tmp.to<JsonObject>();
    e.toJson(obj);
    JsonObjectConst src = obj;
    JsonObject row = rows.createNestedObject();
    fillEntryRow(src, row);
  });
}
