#include <MdnsService.h>

#include <AsyncJson.h>
#include <FormBuilder.h>
#include <WebManager.h>
#include <DeviceIdentity.h>


#define MDNS_FORM_PATH "/rest/system/mdns"

namespace {
// Strip everything that's not a-z 0-9 - and lowercase. mDNS hostnames
// follow LDH-only rules (RFC 1035 / RFC 6762). The canonical device id
// already only has letters, digits, dashes — this guard catches the
// case where someone uses non-ASCII in PROJECT_NAME via factory_settings.
String sanitizeHostname(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
      out += c;
    } else if (c == '_' || c == ' ' || c == '.' || c == '/') {
      // Replace common separators with '-' instead of dropping —
      // makes "esprack-demo-aa:bb..." stay readable.
      if (out.length() > 0 && out[out.length() - 1] != '-') out += '-';
    }
  }
  // Strip trailing '-' so we don't broadcast as "foo-.local".
  while (out.length() > 0 && out[out.length() - 1] == '-') {
    out.remove(out.length() - 1);
  }
  // mDNS hostname soft-limit ~63 chars; truncate from the right.
  if (out.length() > 63) out.remove(63);
  return out;
}
}  // namespace

MdnsService::MdnsService(AsyncWebServer* server) : _server(server) {}

void MdnsService::registerManifest(WebManager* web) {
  if (!web) return;

  // REST endpoint — form schema for the System > mDNS tab. The path
  // matches what tab.restPath declares below, so the dynamic UI
  // fetches the same JSON shape on tab-open.
  WebTabSpec tab;
  tab.key       = "mdns";
  tab.title     = "mDNS";
  tab.restPath  = MDNS_FORM_PATH;
  tab.postable  = false;
  tab.live      = true;
  tab.auth      = WebAuthLevel::Authenticated;
  tab.order     = 25;  // sits after Identity (10) + Resources (12)
  web->addTabToFeature("system", tab);

  if (_server) {
    _server->on(MDNS_FORM_PATH, HTTP_GET,
                [this](AsyncWebServerRequest* r) {
      this->serveForm(r);
    });
  }

  // ── Actions ──────────────────────────────────────────────────────
  // mdns.resolveHost — takes ?host=foo.local, returns the IP via the
  // action's success message so the snackbar shows it inline.
  WebActionSpec resolveAct;
  resolveAct.id              = "mdns.resolveHost";
  resolveAct.title           = "Resolve";
  resolveAct.icon            = "Search";
  resolveAct.color           = "primary";
  resolveAct.auth            = WebAuthLevel::Authenticated;
  resolveAct.successMessage  = "Resolved";
  resolveAct.handler = [this](AsyncWebServerRequest* r) {
    String host;
    if (r->hasArg("host")) host = r->arg("host");
    host.trim();
    if (host.length() == 0) {
      r->send(400, "application/json",
              "{\"ok\":false,\"message\":\"Provide a hostname to resolve (e.g. mothership.local)\"}");
      return;
    }
    IPAddress ip = this->resolveHost(host, 3000);
    if (ip == IPAddress((uint32_t)0)) {
      String msg = String("{\"ok\":false,\"message\":\"") +
                   host + " did not resolve via mDNS\"}";
      r->send(404, "application/json", msg);
      return;
    }
    // Cache so the table row reflects it immediately on form reload.
    _lastResolve.host   = host;
    _lastResolve.ip     = ip;
    _lastResolve.whenMs = millis();

    String msg = String("{\"ok\":true,\"message\":\"") +
                 host + " -> " + ip.toString() + "\"}";
    r->send(200, "application/json", msg);
  };
  web->registerAction(resolveAct);

  // mdns.refreshServices — re-runs browse for the well-known service
  // types ESP-Rack devices and the mothership stand-in advertise.
  //
  // ESPmDNS::queryService blocks ~3 seconds per call (no async API
  // in the Arduino wrapper). Three back-to-back calls = ~9 seconds
  // blocked. Running that on the AsyncWebServer thread (CPU 0)
  // overruns the Task WDT and panics async_tcp. So we spawn a small
  // FreeRTOS task to do the blocking I/O and return 200 to the UI
  // immediately. The form re-fetch on the operator's next refetch
  // picks up the updated cache.
  WebActionSpec refreshAct;
  refreshAct.id              = "mdns.refreshServices";
  refreshAct.title           = "Refresh services";
  refreshAct.icon            = "Refresh";
  refreshAct.color           = "primary";
  refreshAct.auth            = WebAuthLevel::Authenticated;
  refreshAct.successMessage  = "mDNS browse started — refresh tab in a few seconds";
  refreshAct.handler = [this](AsyncWebServerRequest* r) {
    if (_browseTaskRunning) {
      r->send(409, "application/json",
              "{\"ok\":false,\"message\":\"Browse already running — wait for current scan to finish\"}");
      return;
    }
    _browseTaskRunning = true;
    xTaskCreate([](void* arg) {
      auto* self = static_cast<MdnsService*>(arg);
      std::vector<ServiceEntry> all;
      auto extend = [&](const String& svc, const String& proto) {
        auto items = self->browseService(svc, proto, 1500);
        for (auto& it : items) all.push_back(it);
      };
      extend("_esprack", "_tcp");
      extend("_https",   "_tcp");
      extend("_http",    "_tcp");
      self->_browseCache   = std::move(all);
      self->_browseCacheMs = millis();
      self->_browseTaskRunning = false;
      log_i("[mdns] browse task done: %u cached entry(ies)",
                    (unsigned)self->_browseCache.size());
      vTaskDelete(NULL);
    }, "mdns-browse", 4096, this, 1, nullptr);
    r->send(200, "application/json", "{\"ok\":true}");
  };
  web->registerAction(refreshAct);

  // mdns.reinit — forced MDNS.end()+begin(). For when the LAN
  // is fine but the responder went quiet (rare; usually a WiFi
  // mode flip).
  WebActionSpec reinitAct;
  reinitAct.id              = "mdns.reinit";
  reinitAct.title           = "Reinit responder";
  reinitAct.icon            = "RestartAlt";
  reinitAct.color           = "warning";
  reinitAct.auth            = WebAuthLevel::Authenticated;
  reinitAct.successMessage  = "mDNS responder reinitialised";
  reinitAct.handler = [this](AsyncWebServerRequest* r) {
    bool ok = this->initResponder();
    if (ok) {
      r->send(200, "application/json", "{\"ok\":true}");
    } else {
      r->send(500, "application/json",
              "{\"ok\":false,\"message\":\"MDNS.begin() failed — see Serial\"}");
    }
  };
  web->registerAction(reinitAct);
}

void MdnsService::begin() {
  _hostname = computeHostname();
  // Don't try to init yet — WiFi may not be up. loop() will retry
  // until STA has an IP.
}

void MdnsService::loop() {
  if (_active) {
    // mDNS dies cleanly when WiFi drops. Detect the transition so the
    // next reconnect re-init the responder; without this, a 30s WiFi
    // outage leaves us silent on the LAN forever.
    if (WiFi.status() != WL_CONNECTED) {
      log_d("[mdns] WiFi dropped — marking responder inactive");
      _active = false;
      _lastInitAttemptMs = 0;
    }
    return;
  }
  if (WiFi.status() != WL_CONNECTED) return;
  // WL_CONNECTED says "associated to AP" — IP may still be 0.0.0.0
  // until DHCP returns. MDNS.begin() needs a valid local IP for the
  // outgoing announce packet; calling it too early either succeeds
  // silently with bind-to-0.0.0.0 (no replies on the LAN) or fails
  // hard. Wait for an actual non-zero IP.
  IPAddress ip = WiFi.localIP();
  if (ip == IPAddress((uint32_t)0)) return;

  uint32_t now = millis();
  if (_lastInitAttemptMs != 0 &&
      now - _lastInitAttemptMs < _initBackoffMs) {
    return;
  }
  _lastInitAttemptMs = now;
  log_i("[mdns] WiFi up at %s — initialising responder",
                ip.toString().c_str());
  initResponder();
}

bool MdnsService::initResponder() {
  if (_hostname.length() == 0) _hostname = computeHostname();

  // Call end() ONLY if our previous begin() succeeded. Without that
  // guard a double-free path opens: a WiFi-STA disconnect event
  // tears down lwip's mDNS internals, but our `_active` was already
  // flipped to false in loop() so this initResponder pass would
  // call MDNS.end() over already-freed structures → heap poisoning
  // → multi_heap_poisoning panic on the next allocation. With the
  // guard, end() runs only when we know there's live state to tear
  // down.
  if (_active) {
    MDNS.end();
    _active = false;
  }

  if (!MDNS.begin(_hostname.c_str())) {
    _active = false;
    if (_initBackoffMs < 30000) _initBackoffMs *= 2;
    log_e("[mdns] MDNS.begin('%s') failed; retry in %u ms",
                  _hostname.c_str(), (unsigned)_initBackoffMs);
    return false;
  }

  // Advertise ourselves so peers can browse for ESP-Rack devices.
  // _esprack._tcp is our reserved service type; port 80 is the web
  // UI port (still HTTP today; flip to 443 / _esprack-tls once UI
  // moves to HTTPS).
  MDNS.addService("_esprack", "_tcp", 80);
  MDNS.addServiceTxt("_esprack", "_tcp", "device_id",
                     DeviceIdentity::canonical().c_str());
  MDNS.addServiceTxt("_esprack", "_tcp", "project",
                     DeviceIdentity::projectName().c_str());
  MDNS.addServiceTxt("_esprack", "_tcp", "fw",
                     DeviceIdentity::version().c_str());

  _active        = true;
  _initBackoffMs = 2000;  // reset backoff for the next re-init pass
  log_d("[mdns] active as %s.local + _esprack._tcp:80",
                _hostname.c_str());
  return true;
}

IPAddress MdnsService::resolveHost(const String& host, uint32_t timeout_ms) {
  if (!_active) {
    log_d("[mdns] resolveHost('%s'): responder inactive",
                  host.c_str());
    return IPAddress((uint32_t)0);
  }
  // ESPmDNS strips the ".local" suffix for queryHost; do the same so
  // operators can paste either form.
  String name = host;
  if (name.endsWith(".local")) {
    name.remove(name.length() - 6);
  }
  uint32_t t0 = millis();
  IPAddress ip = MDNS.queryHost(name.c_str(), timeout_ms);
  log_d("[mdns] resolveHost('%s' -> '%s'): %s (took %u ms)",
                host.c_str(), name.c_str(),
                ip == IPAddress((uint32_t)0) ? "MISS" : ip.toString().c_str(),
                (unsigned)(millis() - t0));
  return ip;
}

std::vector<IMdnsResolver::ServiceEntry>
MdnsService::browseService(const String& service,
                            const String& proto,
                            uint32_t timeout_ms) {
  std::vector<ServiceEntry> out;
  if (!_active) {
    log_d("[mdns] browseService('%s.%s'): responder inactive",
                  service.c_str(), proto.c_str());
    return out;
  }
  String svc = service;
  if (svc.length() > 0 && svc[0] != '_') svc = String("_") + svc;
  String pr = proto;
  if (pr.length() > 0 && pr[0] != '_') pr = String("_") + pr;

  uint32_t t0 = millis();
  int n = MDNS.queryService(svc.c_str(), pr.c_str());
  // arduino-esp32 queryService blocks ~3s synchronously regardless
  // of the timeout we'd like to apply, so we just take what it gives.
  (void)timeout_ms;
  log_d("[mdns] browseService('%s.%s'): %d instance(s) in %u ms",
                svc.c_str(), pr.c_str(), n,
                (unsigned)(millis() - t0));
  for (int i = 0; i < n; ++i) {
    ServiceEntry e;
    e.instance = MDNS.hostname(i);
    // arduino-esp32 3.x renamed MDNS.IP(int) to MDNS.address(int).
    // Older 2.x had IP() — if we ever need to build against both,
    // an #ifdef guard on ESP_ARDUINO_VERSION would pick.
    e.ip       = MDNS.address(i);
    e.port     = MDNS.port(i);
    // TXT records aren't exposed cleanly by ESPmDNS — leave empty
    // for now; future enhancement could pull via mdns_txt_item_t.
    out.push_back(std::move(e));
  }
  return out;
}

String MdnsService::computeHostname() const {
  // canonical = "<project>-<mac>-<uid8>" — fine for LDH after sanitise.
  // Use short form so hostname stays under 63 chars even with long
  // project names.
  String base = DeviceIdentity::projectName() + "-" +
                DeviceIdentity::macHex12();
  String s = sanitizeHostname(base);
  if (s.length() == 0) s = "esprack";
  return s;
}

void MdnsService::buildForm(JsonObject& root) {
  JsonArray fields = FormBuilder::createForm(root, "mdns", "mDNS responder");

  FormBuilder::addTextField(fields, "hostname", AF::R,
      (_hostname + ".local").c_str(),
      label("This device"), icon("DnsOutlined"));
  FormBuilder::addTextField(fields, "status", AF::R,
      (_active ? "Active" : "Stopped"),
      label("Responder"), icon("CheckCircleOutline"),
      colorMap("Active:success,Stopped:warning,default:info"));

  FormBuilder::addMessageField(fields, "m_resolve_help",
      "Type a concrete hostname such as 'mothership.local' or "
      "'living-room-pi.local' and press Resolve. mDNS does not support "
      "wildcards — entering '*.local' literally will fail. The "
      "discovered IP is shown in the snackbar and cached in the row "
      "below.",
      level("info"), icon("Info"),
      label("How to resolve"));
  FormBuilder::addTextField(fields, "resolve_host", AF::RW, "",
      label("Hostname"),
      placeholder("mothership.local"),
      icon("Search"));
  FormBuilder::addActionField(fields, "a_resolve", "Resolve", AF::RW,
      actionRef("mdns.resolveHost"),
      withFields("host=resolve_host"),
      icon("Search"), color("primary"));

  if (_lastResolve.host.length() > 0) {
    String row = _lastResolve.host + " -> " + _lastResolve.ip.toString();
    FormBuilder::addTextField(fields, "last_resolve", AF::R, row.c_str(),
        label("Last resolved"), icon("History"));
  }

  FormBuilder::addActionField(fields, "a_refresh_services",
      "Refresh services", AF::RW,
      actionRef("mdns.refreshServices"),
      icon("Refresh"), color("primary"), refetchForm());

  JsonObject tbl = FormBuilder::addTableField(fields, "services", AF::R,
      col("instance", "Instance", "text"),
      col("ip",       "IP",       "text"),
      col("port",     "Port",     "text"),
      icon("Apps"));
  JsonArray rows = tbl["services"].as<JsonArray>();
  for (const auto& s : _browseCache) {
    JsonObject r = rows.createNestedObject();
    r["instance"] = s.instance;
    r["ip"]       = s.ip.toString();
    r["port"]     = s.port;
  }

  FormBuilder::addActionField(fields, "a_reinit", "Reinit responder",
      AF::RW, actionRef("mdns.reinit"),
      icon("RestartAlt"), color("warning"));
  FormBuilder::addMessageField(fields, "m_caveat",
      "mDNS is a multicast / link-local protocol. Some corporate APs "
      "filter multicast between SSIDs or block it entirely — if "
      "browse always returns 0 services, your network may be one of "
      "those. Fall back to typing an IP in the consumer modules' URL "
      "fields.",
      level("warning"), icon("Warning"));
}

void MdnsService::serveForm(AsyncWebServerRequest* request) {
  AsyncJsonResponse* response = new AsyncJsonResponse(false, 8192);
  JsonObject root = response->getRoot();
  buildForm(root);
  response->setLength();
  request->send(response);
}
