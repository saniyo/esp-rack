#include <WebEndpointsService.h>

#include <FormBuilder.h>
#include <WebManager.h>
#include <WsManager.h>

WebEndpointsService::WebEndpointsService(AsyncWebServer* server,
                                         SecurityManager* securityManager,
                                         WebManager* webManager,
                                         WsManager* wsManager)
    : _server(server), _sm(securityManager), _wm(webManager), _ws(wsManager) {
  if (!_server || !_sm) return;
  _server->on(WEB_ENDPOINTS_FORM_PATH,
              HTTP_GET,
              _sm->wrapRequest(std::bind(&WebEndpointsService::serveForm, this, std::placeholders::_1),
                               AuthenticationPredicates::IS_ADMIN));
}

void WebEndpointsService::registerManifest(WebManager* web) {
  if (!web) return;
  WebTabSpec tab;
  tab.key      = "endpoints";
  tab.title    = "Endpoints";
  tab.restPath = WEB_ENDPOINTS_FORM_PATH;
  tab.postable = false;
  tab.live     = false;
  tab.auth     = WebAuthLevel::Admin;
  web->addTabToFeature("system", tab);
}

void WebEndpointsService::serveForm(AsyncWebServerRequest* request) {
  AsyncJsonResponse* response = new AsyncJsonResponse(false, WEB_ENDPOINTS_BUFFER_SIZE);
  JsonObject root = response->getRoot();
  buildForm(root);
  response->setLength();
  request->send(response);
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

const char* kindLabel(const char* k) {
  if (!k || !*k) return "?";
  return k;
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
    StaticJsonDocument<4096> stats;
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
  // the same surface that backs /rest/uiManifest.
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
  _wm->forEachEntry([&](const IWebFeatureEntry& e) {
    DynamicJsonDocument tmp(2048);
    JsonObject obj = tmp.to<JsonObject>();
    e.toJson(obj);
    // JsonObject → JsonObjectConst: implicit conversion. Avoid the
    // explicit obj.as<JsonObjectConst>() form — the ArduinoJson 6.21.6
    // template member resolution doesn't pick it up on the pioarduino
    // RISC-V toolchain (the same `is<JsonObject>()` quirk family).
    JsonObjectConst src = obj;

    JsonObject row = rows.createNestedObject();
    row["id"]    = j(src, "id");
    row["kind"]  = kindLabel(j(src, "kind").c_str());

    // Feature: rest.read / rest.update  +  ws  (ws sometimes flat, sometimes nested)
    // Action:  restPath  +  method
    // Page:    restPath  (component owns its own endpoint list)
    // Info:    rest.read (read-only stateless feature reader)
    String read   = jNested(src, "rest", "read");
    if (read.length() == 0) read = j(src, "restPath");
    String write  = jNested(src, "rest", "update");
    String ws     = j(src, "ws");
    String method = j(src, "method");
    String auth   = j(src, "auth");

    row["read"]   = read;
    row["write"]  = write;
    row["ws"]     = ws;
    row["method"] = method;
    row["auth"]   = auth;
  });
}
