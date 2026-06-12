#include <MeshService.h>

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "mdns.h"
#include "mbedtls/md.h"

#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <ProblemJson.h>
#include <WebManager.h>
#include <WebFeatureSpec.h>
#include <DeviceIdentity.h>
#include <Version.h>

// ── Tunables ───────────────────────────────────────────────────────────
#ifndef FACTORY_MESH_PSK
// The "internal key" peers use to recognise each other. THE DEFAULT IS A
// PLACEHOLDER shared by every stock build — fine for a bench test where all
// devices run the same firmware. A real deployment MUST override it
// (-D FACTORY_MESH_PSK=\"...\") so only your fleet trusts each other.
// (Production path: distribute a per-fleet PSK via the mothership enroll.)
#define FACTORY_MESH_PSK "esprack-mesh-dev"
#endif

// Manifest envelope schema this build speaks — MUST stay in sync with
// WebManager's `schemaVersion`. A peer whose schema matches is renderable by
// our web ("a web version we support"), regardless of its project.
#define MESH_SCHEMA_VERSION "3"

namespace {
const char*    PEERS_PATH              = "/api/v1/mesh/peers";
const uint32_t MESH_BROWSE_INTERVAL_MS = 20000;  // re-browse cadence
const uint32_t MESH_QUERY_TIMEOUT_MS   = 3000;   // per mdns_query_ptr
const size_t   MESH_MAX_PEERS          = 32;     // bounded cache
// 503 guard for /api/v1/mesh/peers — must cover the 4096 response doc
// plus a little slack, no more. The menu polls this path every 10 s, so
// an over-strict guard (was 6144) turned routine C3 heap levels (~9 KB
// maxAlloc minus any in-flight form) into a steady drip of 503 toasts.
const uint32_t MESH_SERVE_MIN_ALLOC    = 4608;

// HMAC-SHA256(key, msg) truncated to `outBytes`, lowercase hex.
String hmacHex(const String& key, const String& msg, size_t outBytes) {
  uint8_t mac[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return String();
  if (mbedtls_md_hmac(info,
                      (const uint8_t*)key.c_str(), key.length(),
                      (const uint8_t*)msg.c_str(), msg.length(), mac) != 0) {
    return String();
  }
  static const char hx[] = "0123456789abcdef";
  if (outBytes > sizeof(mac)) outBytes = sizeof(mac);
  String out;
  out.reserve(outBytes * 2);
  for (size_t i = 0; i < outBytes; ++i) {
    out += hx[mac[i] >> 4];
    out += hx[mac[i] & 0x0F];
  }
  return out;
}
}  // namespace

void MeshService::registerManifest(WebManager* web) {
  if (!web) return;

  // Discovery page — a bespoke React renderer (CUSTOM_RENDERERS["Discovery"])
  // that fetches /api/v1/mesh/peers. Route derived from id => /discovery.
  // In the mesh menu this is the top-level umbrella under which device records nest.
  WebPageSpec page;
  page.id         = "discovery";
  page.title      = "Discovery";
  page.component  = "Discovery";
  page.menu.label = "Discovery";
  page.menu.icon  = "DeviceHub";
  page.menu.order = 1;            // top of the side-nav
  page.menu.auth  = WebAuthLevel::Authenticated;
  page.auth       = WebAuthLevel::Authenticated;

  // GET /api/v1/mesh/peers — discovered + trust-verified peers. The
  // framework auth-wraps the handler via e.auth.
  WebEndpointMeta e;
  e.method  = "GET";
  e.path    = PEERS_PATH;
  e.role    = "peers";
  e.auth    = WebAuthLevel::Authenticated;
  e.handler = [this](AsyncWebServerRequest* r) { this->servePeers(r); };
  page.endpoints.push_back(std::move(e));

  web->registerPage(std::move(page));
}

void MeshService::begin(IMdnsResolver* mdns) {
  _mdns      = mdns;
  _psk       = FACTORY_MESH_PSK;
  _canonical = DeviceIdentity::canonical();
  // Static trust tag: HMAC(PSK, our device id) truncated to 16 bytes. A peer
  // recomputes HMAC(PSK, peer.device_id) and compares — proving we both hold
  // the PSK without ever transmitting it. (Static for v1; a rolling
  // time-window tag is the replay-hardening step.)
  _sig       = hmacHex(_psk, _canonical, 16);
  _mutex     = xSemaphoreCreateMutex();

  // Persistent discovery task. Static stack — the buffer lives in BSS, not
  // heap, so these 5 KB never sit as a permanent hole in the middle of the
  // heap (same P4 pattern as Mothership/Telegram). On the no-PSRAM C3 the
  // post-warmup max_alloc hovers near ~9 KB; a 5 KB heap-resident stack
  // right in that window was measurably starving the 6-8 KB REST response
  // buffers into 503s.
  if (_task == nullptr) {
    constexpr uint32_t kStackWords = 5120 / sizeof(StackType_t);
    static StackType_t  s_taskStack[kStackWords];
    static StaticTask_t s_taskBlock;
    _task = xTaskCreateStatic(&MeshService::taskTramp, "mesh-disc", kStackWords,
                              this, 1, s_taskStack, &s_taskBlock);
    if (_task == nullptr) {
      log_e("[mesh] xTaskCreateStatic failed");
    }
  }

  log_i("[mesh] up id=%s sig=%s key=%s", _canonical.c_str(), _sig.c_str(),
        (_psk == "esprack-mesh-dev") ? "DEFAULT(dev)" : "custom");
}

void MeshService::taskTramp(void* arg) {
  static_cast<MeshService*>(arg)->discoveryLoop();
}

void MeshService::discoveryLoop() {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(MESH_BROWSE_INTERVAL_MS));
    if (!_mdns || !_mdns->isActive()) continue;  // wait for the responder
    ensureAdvert();
    browseAndUpdate();
  }
}

void MeshService::ensureAdvert() {
  // (Re)assert our extra TXT on the mDNS module's _esprack._tcp advert.
  // addServiceTxt SETS (idempotent), so re-running each cycle survives an
  // mDNS re-init after a WiFi drop. device_id/project/fw come from the mDNS
  // module; we add the mesh keys.
  MDNS.addServiceTxt("_esprack", "_tcp", "schema", MESH_SCHEMA_VERSION);
  MDNS.addServiceTxt("_esprack", "_tcp", "fw_fx",  ESPRACK_VERSION_STR);
  MDNS.addServiceTxt("_esprack", "_tcp", "sig",    _sig.c_str());
}

void MeshService::browseAndUpdate() {
  mdns_result_t* results = nullptr;
  esp_err_t err = mdns_query_ptr("_esprack", "_tcp",
                                 MESH_QUERY_TIMEOUT_MS, MESH_MAX_PEERS,
                                 &results);
  if (err != ESP_OK) {
    log_w("[mesh] mdns_query_ptr failed: %d", (int)err);
    return;
  }

  std::vector<Peer> found;
  uint32_t now = millis();
  for (mdns_result_t* r = results; r != nullptr; r = r->next) {
    Peer p;
    for (size_t i = 0; i < r->txt_count; ++i) {
      const char* k = r->txt[i].key;
      const char* v = r->txt[i].value ? r->txt[i].value : "";
      if      (!strcmp(k, "device_id")) p.deviceId  = v;
      else if (!strcmp(k, "project"))   p.project   = v;
      else if (!strcmp(k, "fw"))        p.fw        = v;
      else if (!strcmp(k, "schema"))    p.schema    = v;
      else if (!strcmp(k, "fw_fx"))     p.framework = v;
      else if (!strcmp(k, "sig"))       p.sig       = v;
    }
    // Skip self and any unidentified responder.
    if (p.deviceId.length() == 0 || p.deviceId == _canonical) continue;

    for (mdns_ip_addr_t* a = r->addr; a != nullptr; a = a->next) {
      if (a->addr.type == ESP_IPADDR_TYPE_V4) {
        p.ip = IPAddress(a->addr.u_addr.ip4.addr);
        break;
      }
    }
    p.port       = r->port;
    p.trusted    = p.sig.length() && (hmacHex(_psk, p.deviceId, 16) == p.sig);
    p.compatible = (p.schema == MESH_SCHEMA_VERSION);
    p.lastSeenMs = now;
    found.push_back(std::move(p));
    if (found.size() >= MESH_MAX_PEERS) break;
  }
  if (results) mdns_query_results_free(results);

  // Swap the snapshot in under the lock — held only for the quick move.
  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
  _peers = std::move(found);
  size_t n = _peers.size();
  if (_mutex) xSemaphoreGive(_mutex);
  log_i("[mesh] %u peer(s) on the LAN", (unsigned)n);
}

void MeshService::servePeers(AsyncWebServerRequest* request) {
  // Tolerance guard — refuse rather than OOM-panic under heap pressure.
  if (ESP.getMaxAllocHeap() < MESH_SERVE_MIN_ALLOC) {
    sendProblem(request, 503, "heap_pressure",
                "Peer roster needs more contiguous heap than is free", 2);
    return;
  }

  AsyncJsonResponse* response = new AsyncJsonResponse(false, 4096);
  JsonObject root = response->getRoot();

  JsonObject self = root.createNestedObject("self");
  self["device_id"] = _canonical.length() ? _canonical : DeviceIdentity::canonical();
  self["project"]   = DeviceIdentity::projectName();
  self["fw"]        = DeviceIdentity::version();
  self["schema"]    = MESH_SCHEMA_VERSION;
  self["framework"] = ESPRACK_VERSION_STR;
  self["ip"]        = WiFi.localIP().toString();

  JsonArray arr = root.createNestedArray("peers");
  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
  uint32_t now = millis();
  for (const auto& p : _peers) {
    JsonObject o = arr.createNestedObject();
    o["device_id"]  = p.deviceId;
    o["project"]    = p.project;
    o["fw"]         = p.fw;
    o["schema"]     = p.schema;
    o["framework"]  = p.framework;
    o["ip"]         = p.ip.toString();
    o["port"]       = p.port;
    o["trusted"]    = p.trusted;
    o["compatible"] = p.compatible;
    o["age_ms"]     = (uint32_t)(now - p.lastSeenMs);
  }
  if (_mutex) xSemaphoreGive(_mutex);

  response->setLength();
  request->send(response);
}
