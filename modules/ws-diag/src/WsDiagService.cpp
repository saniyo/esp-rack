#include <WsDiagService.h>

#include <FormBuilder.h>
#include <PresenceService.h>
#include <WebManager.h>

WsDiagService::WsDiagService(AsyncWebServer* server, SecurityManager* securityManager,
                             WsManager* wsMgr, PresenceService* presence)
    : _server(server), _sm(securityManager), _ws(wsMgr), _presence(presence) {
  if (!_server || !_sm) return;
  _server->on(WS_DIAG_FORM_PATH,
              HTTP_GET,
              _sm->wrapRequest(std::bind(&WsDiagService::socketsForm, this, std::placeholders::_1),
                               AuthenticationPredicates::IS_ADMIN));
}

void WsDiagService::registerManifest(WebManager* web) {
  if (!web) return;
  // Tab renamed sockets→clients; this view now only carries per-client
  // tables (active WS clients + presence-tracked tabs). Aggregate WS
  // health (total clients, avg RTT, keepalive cadence, backpressure
  // counters) moved to the Endpoints tab where roll-ups live.
  WebTabSpec tab;
  tab.key      = "clients";
  tab.title    = "Clients";
  tab.restPath = WS_DIAG_FORM_PATH;
  tab.postable = false;
  tab.live     = true;
  tab.auth     = WebAuthLevel::Admin;
  tab.order    = 20;  // legacy parity: status / clients / config / endpoints / …
  web->addTabToFeature("system", tab);
}

void WsDiagService::socketsForm(AsyncWebServerRequest* request) {
  AsyncJsonResponse* response = new AsyncJsonResponse(false, WS_DIAG_BUFFER_SIZE);
  JsonObject root = response->getRoot();
  buildForm(root);
  response->setLength();
  request->send(response);
}

void WsDiagService::buildForm(JsonObject& root) {
  JsonArray sum = FormBuilder::createForm(root, "clients", "Clients");

  // ---- Presence (by-client) section --------------------------------------
  // One row per logical tab, sourced from PresenceService. This is the
  // "who's connected" view — independent of whether that tab currently has
  // any feature WS open. REST-only visits (e.g. Settings pages) still show
  // up because every axios call hits the SecurityManager wrap and touches
  // the registry.
  StaticJsonDocument<4096> presenceDoc;
  JsonObject presenceRoot = presenceDoc.to<JsonObject>();
  if (_presence) _presence->fillStats(presenceRoot);

  const uint32_t totalPresence = presenceRoot["total_presence"] | 0;
  String presenceSummary = String(totalPresence) + " active tab(s) tracked";
  FormBuilder::addTextField(sum, "presence_total", AF::R, presenceSummary.c_str(),
                            icon("Computer"),
                            color(totalPresence > 0 ? "success" : "info"));

  JsonObject byClient = FormBuilder::addTableField(sum, "by_client", AF::R,
                                                   col("key",          "Key",          "text"),
                                                   col("ip",           "Remote IP",    "text"),
                                                   col("status",       "Status",       "text"),
                                                   col("current_page", "Current Page", "text"),
                                                   col("first_seen_s", "Age (s)",      "number"),
                                                   col("last_rest_s",  "Rest age (s)", "number"),
                                                   col("last_ws_s",    "WS age (s)",   "number"),
                                                   col("page_history", "History",      "text"),
                                                   icon("Devices"));

  JsonArray presRows = byClient["by_client"].as<JsonArray>();
  JsonArray presClients = presenceRoot["clients"].as<JsonArray>();
  for (JsonObject pc : presClients) {
    JsonObject row = presRows.createNestedObject();
    // Only show the first 8 chars of the UUID to keep the cell narrow —
    // full key is still in the raw JSON if anyone needs to correlate.
    String key = pc["key"] | "";
    if (key.length() > 8) key = key.substring(0, 8);
    row["key"]          = key;
    row["ip"]           = pc["ip"]           | "";
    row["status"]       = pc["status"]       | "";
    row["current_page"] = pc["current_page"] | "";
    row["first_seen_s"] = pc["first_seen_s"] | 0;
    row["last_rest_s"]  = pc["last_rest_s"]  | 0;
    row["last_ws_s"]    = pc["last_ws_s"]    | 0;
    row["page_history"] = pc["page_history"] | "";
  }
}
