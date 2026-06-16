#include <WebEndpointsService.h>

#include <AdaptiveJson.h>
#include <FormBuilder.h>
#include <web/FormStream.h>
#include <WebManager.h>
#include <WsManager.h>

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

// Defined in the anonymous namespace below (same TU = same namespace);
// forward-declared here so the streamed serveForm can use it before the
// definition. Maps one registry entry's toJson() into a table row.
namespace { void fillEntryRow(JsonObjectConst src, JsonObject& row); }

void WebEndpointsService::serveForm(AsyncWebServerRequest* request) {
  // STREAMED, not built-then-sent: the registry table (~40 rows) is ~9 KB
  // as one ArduinoJson tree — no contiguous block that big on the C3 under
  // concurrency, so a single-doc build sheds a 503. FormStream emits the
  // form field-by-field / row-by-row over a chunked stream, holding only
  // one row (~1 KB) at a time, so it ALWAYS streams out instead of 503ing
  // on size. (buildForm below stays for the rest.proxy in-process path,
  // where the mothership caller sizes the destination doc.)
  if (!_wm) {
    sendProblem(request, 503, "unavailable", "WebManager not wired up", 5);
    return;
  }
  // Floor: beginResponseStream needs its ~1.5 KB chunk buffer plus a row
  // scratch; below this even streaming can't start.
  if (ESP.getMaxAllocHeap() < 3072) {
    sendProblem(request, 503, "heap_pressure",
                "Too little heap to stream the endpoints list", 2);
    return;
  }

  // Pre-walk for the summary + per-kind breakdown counts.
  size_t total = 0, cFeature = 0, cAction = 0, cPage = 0, cStatus = 0;
  _wm->forEachEntry([&](const IWebFeatureEntry& e) {
    total++;
    switch (e.kind()) {
      case IWebFeatureEntry::Kind::Feature: cFeature++; break;
      case IWebFeatureEntry::Kind::Action:  cAction++;  break;
      case IWebFeatureEntry::Kind::Page:    cPage++;    break;
      case IWebFeatureEntry::Kind::Status:  cStatus++;  break;
    }
  });

  FormStream fs(request);
  fs.begin("endpoints", "Endpoints");

  String totalStr = String(total) + " entries registered";
  fs.field([&](JsonArray& a) {
    FormBuilder::addTextField(a, "total", AF::R, totalStr.c_str(), icon("Apps"));
  });
  String mix = String("features: ") + cFeature + ", actions: " + cAction +
               ", pages: " + cPage + ", info: " + cStatus;
  fs.field([&](JsonArray& a) {
    FormBuilder::addTextField(a, "breakdown", AF::R, mix.c_str(), icon("Dashboard"));
  });

  if (_ws) {
    DynamicJsonDocument stats(2048);  // WS client roster is small + bounded
    JsonObject statsRoot = stats.to<JsonObject>();
    _ws->fillStats(statsRoot);

    const uint32_t totalClients   = statsRoot["total_clients"] | 0;
    const float    avgRtt         = statsRoot["avg_rtt_ms"]    | 0.0f;
    const uint32_t pingIvl        = statsRoot["ping_interval_s"] | 0;
    const uint32_t pongTo         = statsRoot["pong_timeout_ms"] | 0;
    const uint32_t totalDrops     = statsRoot["total_dropped"] | 0;
    const uint32_t totalBcstDrops = statsRoot["total_broadcasts_dropped"] | 0;
    const uint32_t thr            = statsRoot["backpressure_threshold"]   | 0;
    const uint32_t maxQ           = statsRoot["backpressure_max"]         | 0;

    String wsClientStr = String(totalClients) + " across " +
                         String(statsRoot["endpoints"].size()) + " endpoints";
    fs.field([&](JsonArray& a) {
      FormBuilder::addTextField(a, "ws_clients", AF::R, wsClientStr.c_str(),
                                icon("Computer"), color(totalClients > 0 ? "success" : "info"));
    });
    String rttStr = String(avgRtt, 1) + " ms";
    fs.field([&](JsonArray& a) {
      FormBuilder::addTextField(a, "ws_avg_rtt", AF::R, rttStr.c_str(), icon("NetworkPing"));
    });
    String keepaliveStr = String("ping every ") + String(pingIvl) + " s, " +
                          "evict after " + String(pongTo / 1000UL) + " s silence";
    fs.field([&](JsonArray& a) {
      FormBuilder::addTextField(a, "ws_keepalive", AF::R, keepaliveStr.c_str(), icon("Sync"));
    });
    String bpStr = String(totalDrops) + " per-client / " + String(totalBcstDrops) +
                   " whole-broadcast, threshold " + String(thr) + "/" + String(maxQ);
    fs.field([&](JsonArray& a) {
      FormBuilder::addTextField(a, "ws_backpressure", AF::R, bpStr.c_str(),
                                icon("Warning"), color((totalDrops || totalBcstDrops) ? "warning" : "info"));
    });

    fs.beginTable([&](JsonArray& a) {
      FormBuilder::addTableField(a, "ws_clients_table", AF::R,
          col("path", "Endpoint", "text"), col("id", "ID", "number", format("0")),
          col("ip", "Remote IP", "text"), col("age_s", "Age (s)", "number", format("0")),
          col("last_rx_s", "Rx age (s)", "number", format("0")),
          col("last_tx_s", "Tx age (s)", "number", format("0")),
          col("drops", "Drops", "number", format("0")), icon("Storage"));
    });
    JsonArray eps = statsRoot["endpoints"].as<JsonArray>();
    for (JsonObject epObj : eps) {
      const char* path = epObj["path"] | "";
      JsonArray cl = epObj["clients"].as<JsonArray>();
      for (JsonObject c : cl) {
        fs.row([&](JsonObject& row) {
          row["path"]      = path;
          row["id"]        = c["id"]        | 0;
          row["ip"]        = c["ip"]        | "";
          row["age_s"]     = c["age_s"]     | 0;
          row["last_rx_s"] = c["last_rx_s"] | 0;
          row["last_tx_s"] = c["last_tx_s"] | 0;
          row["drops"]     = c["drops"]     | 0;
        });
      }
    }
    fs.endTable();
  }

  // Per-entry registry table — the big part, streamed row by row.
  fs.beginTable([&](JsonArray& a) {
    FormBuilder::addTableField(a, "entries", AF::R,
        col("id", "ID", "text"), col("kind", "Kind", "text"), col("read", "REST GET", "text"),
        col("write", "REST POST", "text"), col("ws", "WS", "text"), col("method", "Method", "text"),
        col("auth", "Auth", "text"), icon("ListAlt"));
  });
  DynamicJsonDocument tmp(2048);  // reused per entry; freed each iteration
  _wm->forEachEntry([&](const IWebFeatureEntry& e) {
    tmp.clear();
    JsonObject obj = tmp.to<JsonObject>();
    e.toJson(obj);
    JsonObjectConst src = obj;
    fs.row([&](JsonObject& row) { fillEntryRow(src, row); });
  });
  fs.endTable();
  fs.end();
}

namespace {

// Pull a field out of an entry's toJson() snapshot, returning empty
// String if missing. Wraps the JsonObject lookup so the buildForm body
// stays readable.
String j(JsonObjectConst src, const char* key) {
  JsonVariantConst v = src[key];
  if (!v.is<const char*>() && !v.is<String>()) return String();
  return v.as<String>();
}

// Same for nested rest{read,update}: the two typed-feature endpoints
// live under a sub-object; actions and the rest expose them flat.
String jNested(JsonObjectConst src, const char* objKey, const char* leafKey) {
  JsonObjectConst inner = src[objKey].as<JsonObjectConst>();
  if (inner.isNull()) return String();
  JsonVariantConst v = inner[leafKey];
  if (!v.is<const char*>() && !v.is<String>()) return String();
  return v.as<String>();
}

// Map one registry entry's toJson() snapshot (`src`) into a table `row`.
// Shared by the streamed serveForm (HTTP) and buildForm (rest.proxy) so
// the column mapping can never diverge between the two paths.
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

}  // namespace

void WebEndpointsService::buildForm(JsonObject& root) {
  JsonArray ep = FormBuilder::createForm(root, "endpoints", "Endpoints");

  if (!_wm) {
    FormBuilder::addMessageField(ep, "err",
        "WebManager is not wired up.",
        level("error"), icon("Warning"));
    return;
  }

  // Pre-walk: count totals + per-kind breakdown so the header rows render
  // colour-coded badges before the table.
  size_t total = 0;
  size_t cFeature = 0, cAction = 0, cPage = 0, cStatus = 0;
  _wm->forEachEntry([&](const IWebFeatureEntry& e) {
    total++;
    switch (e.kind()) {
      case IWebFeatureEntry::Kind::Feature: cFeature++; break;
      case IWebFeatureEntry::Kind::Action:  cAction++;  break;
      case IWebFeatureEntry::Kind::Page:    cPage++;    break;
      case IWebFeatureEntry::Kind::Status:  cStatus++;  break;
    }
  });

  // Summary row
  String totalStr = String(total) + " entries registered";
  FormBuilder::addTextField(ep, "total", AF::R, totalStr.c_str(),
                            icon("Apps"));

  // Breakdown row — one composite line so the operator sees the mix at a glance.
  String mix = String("features: ") + cFeature
             + ", actions: "        + cAction
             + ", pages: "          + cPage
             + ", info: "           + cStatus;
  FormBuilder::addTextField(ep, "breakdown", AF::R, mix.c_str(),
                            icon("Dashboard"));

  // ---- WS aggregate health (moved from old "Sockets" tab) ----------------
  // Per-client stats now live on the Clients tab; this Endpoints tab
  // surfaces only roll-ups so an operator can answer "is the system
  // healthy" without scrolling through individual client rows. Same data
  // source as the Clients tab uses for its rows (WsManager::fillStats),
  // we just project the headline metrics here.
  if (_ws) {
    // Heap, not StaticJsonDocument: a large doc on the stack of the
    // async_tcp task left no headroom for FormBuilder frames below —
    // stack-overflow territory. sendAdaptiveJson owns the heap-pressure
    // gate so this allocation fits when we get here.
    DynamicJsonDocument stats(4096);
    JsonObject statsRoot = stats.to<JsonObject>();
    _ws->fillStats(statsRoot);

    const uint32_t totalClients = statsRoot["total_clients"] | 0;
    const float    avgRtt       = statsRoot["avg_rtt_ms"]    | 0.0f;
    const uint32_t pingIvl      = statsRoot["ping_interval_s"] | 0;
    const uint32_t pongTo       = statsRoot["pong_timeout_ms"] | 0;
    const uint32_t totalDrops     = statsRoot["total_dropped"] | 0;
    const uint32_t totalBcstDrops = statsRoot["total_broadcasts_dropped"] | 0;
    const uint32_t thr            = statsRoot["backpressure_threshold"]   | 0;
    const uint32_t maxQ           = statsRoot["backpressure_max"]         | 0;

    String wsClientStr = String(totalClients) + " across "
                      + String(statsRoot["endpoints"].size()) + " endpoints";
    FormBuilder::addTextField(ep, "ws_clients", AF::R, wsClientStr.c_str(),
                              icon("Computer"),
                              color(totalClients > 0 ? "success" : "info"));

    String rttStr = String(avgRtt, 1) + " ms";
    FormBuilder::addTextField(ep, "ws_avg_rtt", AF::R, rttStr.c_str(),
                              icon("NetworkPing"));

    String keepaliveStr = String("ping every ") + String(pingIvl) + " s, "
                        + "evict after " + String(pongTo / 1000UL) + " s silence";
    FormBuilder::addTextField(ep, "ws_keepalive", AF::R, keepaliveStr.c_str(),
                              icon("Sync"));

    String bpStr = String(totalDrops) + " per-client / "
                 + String(totalBcstDrops) + " whole-broadcast, "
                 + "threshold " + String(thr) + "/" + String(maxQ);
    FormBuilder::addTextField(ep, "ws_backpressure", AF::R, bpStr.c_str(),
                              icon("Warning"),
                              color((totalDrops || totalBcstDrops) ? "warning" : "info"));

    // ---- Active WS clients per endpoint ---------------------------------
    // Per-(endpoint, client) roster. Lives here on Endpoints because it's
    // intrinsically endpoint-keyed — every row's "Endpoint" column points
    // at one of the WS feature endpoints listed in the registry table
    // below. The Clients tab keeps presence (logical-tab) information,
    // which is connection-agnostic.
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

  // Per-entry table — populated inline from IWebFeatureEntry::toJson(),
  // the same surface that backs /api/v1/ui/manifest.
  JsonObject tbl = FormBuilder::addTableField(ep, "entries", AF::R,
      col("id",     "ID",      "text"),
      col("kind",   "Kind",    "text"),
      col("read",   "REST GET","text"),
      col("write",  "REST POST","text"),
      col("ws",     "WS",      "text"),
      col("method", "Method",  "text"),
      col("auth",   "Auth",    "text"),
      icon("ListAlt"));

  JsonArray rows = tbl["entries"].as<JsonArray>();
  // ONE scratch doc reused across all entries (clear() resets the pool).
  // Row mapping shared with the streamed serveForm via fillEntryRow so the
  // two paths can't diverge. NOTE: this single-doc builder is the
  // rest.proxy path only; the HTTP GET path streams (serveForm).
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
