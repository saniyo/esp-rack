#include <DiscoveryService.h>

#include <Arduino.h>
#include <WiFi.h>
#include "mbedtls/md.h"

#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <AdaptiveJson.h>
#include <ProblemJson.h>
#include <WebManager.h>
#include <WebFeatureSpec.h>
#include <DeviceIdentity.h>
#include <Version.h>

// ── Tunables ───────────────────────────────────────────────────────────
#ifndef FACTORY_DISCOVERY_PSK
// The "internal key" peers use to recognise each other. THE DEFAULT IS A
// PLACEHOLDER shared by every stock build — fine for a bench test where all
// devices run the same firmware. A real deployment MUST override it
// (-D FACTORY_DISCOVERY_PSK=\"...\") so only your fleet trusts each other.
// (Production path: distribute a per-fleet PSK via the mothership enroll.)
#define FACTORY_DISCOVERY_PSK "esprack-discovery-dev"
#endif

// Manifest envelope schema this build speaks — MUST stay in sync with
// WebManager's `schemaVersion`. A peer whose schema matches is renderable by
// our web ("a web version we support"), regardless of its project.
#define DISCOVERY_SCHEMA_VERSION "3"

namespace {
const char*    PEERS_PATH            = "/api/v1/discovery/peers";
// Org-local (239.x) multicast group + port for the discovery membership
// protocol. See modules/discovery/DISCOVERY_PROTOCOL.md §2.1.
const char*    DISCOVERY_GROUP       = "239.42.7.1";
const uint16_t DISCOVERY_PORT        = 7711;
const uint32_t ANNOUNCE_INTERVAL_MS  = 8000;    // self-announce cadence
const uint32_t PEER_TIMEOUT_MS       = 30000;   // ~3.75 missed announces -> drop
const uint32_t RX_IDLE_DELAY_MS      = 50;      // poll yield when no packet
const size_t   DISCOVERY_MAX_PEERS   = 64;      // bounded table (headroom over 50)
const size_t   MAX_PKT               = 320;     // one announce fits comfortably
const char     PKT_MAGIC             = 'E';     // "E5|2|..." wire prefix
const uint8_t  PKT_VER               = 2;

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

void DiscoveryService::registerManifest(WebManager* web) {
  if (!web) return;

  // Discovery page — a bespoke React renderer (CUSTOM_RENDERERS["Discovery"])
  // that fetches /api/v1/discovery/peers. Route derived from id => /discovery.
  // In the device menu this is the top-level umbrella under which device records nest.
  WebPageSpec page;
  page.id         = "discovery";
  page.title      = "Discovery";
  page.component  = "Discovery";
  page.menu.label = "Discovery";
  page.menu.icon  = "DeviceHub";
  page.menu.order = 1;            // top of the side-nav
  page.menu.auth  = WebAuthLevel::Authenticated;
  page.auth       = WebAuthLevel::Authenticated;

  // GET /api/v1/discovery/peers — discovered + trust-verified peers. The
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

void DiscoveryService::begin() {
  _psk       = FACTORY_DISCOVERY_PSK;
  _canonical = DeviceIdentity::canonical();
  // Static trust tag: HMAC(PSK, our device id) truncated to 16 bytes. A peer
  // recomputes HMAC(PSK, peer.device_id) and compares — proving we both hold
  // the PSK without ever transmitting it. (Static for v1; a rolling per-packet
  // HMAC + epoch is the replay-hardening step — see DISCOVERY_PROTOCOL.md §6.)
  _sig       = hmacHex(_psk, _canonical, 16);
  _mutex     = xSemaphoreCreateMutex();
  // Deliberately NOT reserve()d: reserve(MAX_PEERS) of the String-bearing Peer
  // is a ~7 KB contiguous HEAP block that pins C3 max_alloc into the 6-8 KB 503
  // zone — i.e. it eats the very headroom that removing the mDNS responder was
  // meant to free (HW-measured: max_alloc stuck at 7668 with the reserve). The
  // vector grows on demand instead (a handful of peers on a bench). The fixed
  // BSS PeerRow table (DISCOVERY_PROTOCOL.md §4) is the real 50+ determinism fix.

  // Persistent discovery task. Static stack — the buffer lives in BSS, not
  // heap, so it never sits as a hole that FRAGMENTS the heap (same P4 pattern
  // as Mothership/Telegram). 4096 B covers the WiFiUDP recv + parse + the
  // String upsert; re-measure stack_free in System->Resources if the protocol
  // grows (the SWIM probing phase will add a little).
  if (_task == nullptr) {
    constexpr uint32_t kStackWords = 4096 / sizeof(StackType_t);
    static StackType_t  s_taskStack[kStackWords];
    static StaticTask_t s_taskBlock;
    _task = xTaskCreateStatic(&DiscoveryService::taskTramp, "discovery", kStackWords,
                              this, 1, s_taskStack, &s_taskBlock);
    if (_task == nullptr) {
      log_e("[discovery] xTaskCreateStatic failed");
    }
  }

  log_i("[discovery] up id=%s sig=%s key=%s", _canonical.c_str(), _sig.c_str(),
        (_psk == "esprack-discovery-dev") ? "DEFAULT(dev)" : "custom");
}

void DiscoveryService::taskTramp(void* arg) {
  static_cast<DiscoveryService*>(arg)->discoveryLoop();
}

bool DiscoveryService::openSocket() {
  // beginMulticast joins the group AND binds the listen port in one call.
  IPAddress group;
  group.fromString(DISCOVERY_GROUP);
  if (_udp.beginMulticast(group, DISCOVERY_PORT) != 1) {
    log_w("[discovery] beginMulticast(%s:%u) failed", DISCOVERY_GROUP, DISCOVERY_PORT);
    return false;
  }
  log_i("[discovery] listening on %s:%u", DISCOVERY_GROUP, DISCOVERY_PORT);
  return true;
}

void DiscoveryService::announceSelf() {
  // Wire: "E5|2|<device_id>|<project>|<fw>|<schema>|<fw_fx>|<sig>|<port>".
  // The receiver takes the source IP from the datagram, so we don't send it.
  char buf[MAX_PKT];
  int n = snprintf(buf, sizeof(buf), "%c%u|%u|%s|%s|%s|%s|%s|%s|%u",
                   PKT_MAGIC, (unsigned)5, (unsigned)PKT_VER,
                   _canonical.c_str(),
                   DeviceIdentity::projectName().c_str(),
                   DeviceIdentity::version().c_str(),
                   DISCOVERY_SCHEMA_VERSION,
                   ESPRACK_VERSION_STR,
                   _sig.c_str(),
                   (unsigned)_webPort);
  if (n <= 0 || n >= (int)sizeof(buf)) {
    log_w("[discovery] announce payload truncated (%d)", n);
    return;
  }
  if (_udp.beginMulticastPacket() != 1) {
    log_d("[discovery] beginMulticastPacket failed");
    return;
  }
  _udp.write((const uint8_t*)buf, n);
  _udp.endPacket();
}

void DiscoveryService::processPacket(char* buf, int len, IPAddress src) {
  if (len < 6) return;
  buf[len] = '\0';

  // Split on '|' in place, preserving empty fields (no strtok — it collapses
  // adjacent delimiters and would misalign the columns).
  const int   MAXF = 9;
  const char* f[MAXF];
  int nf = 0;
  f[nf++] = buf;
  for (char* c = buf; *c && nf < MAXF; ++c) {
    if (*c == '|') { *c = '\0'; f[nf++] = c + 1; }
  }
  if (nf < MAXF) return;                       // malformed
  // f[0] = "E5", f[1] = ver
  if (f[0][0] != PKT_MAGIC || f[0][1] != '5') return;
  if ((uint8_t)atoi(f[1]) != PKT_VER) return;

  Peer p;
  p.deviceId  = f[2];
  // Skip self and any unidentified responder.
  if (p.deviceId.length() == 0 || p.deviceId == _canonical) return;
  p.project   = f[3];
  p.fw        = f[4];
  p.schema    = f[5];
  p.framework = f[6];
  p.sig       = f[7];
  p.port      = (uint16_t)atoi(f[8]);
  p.ip        = src;
  // Same trust model as the mDNS-TXT version: recompute HMAC(PSK, device_id)
  // and compare to the advertised tag — proves shared-PSK ownership.
  p.trusted    = p.sig.length() && (hmacHex(_psk, p.deviceId, 16) == p.sig);
  p.compatible = (p.schema == DISCOVERY_SCHEMA_VERSION);
  p.lastSeenMs = millis();

  upsertPeer(std::move(p));
}

void DiscoveryService::upsertPeer(Peer&& p) {
  if (!_mutex) return;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  bool found = false;
  for (auto& e : _peers) {
    if (e.deviceId == p.deviceId) {
      e = std::move(p);          // update in place (ip/fw/sig/lastSeen refresh)
      found = true;
      break;
    }
  }
  if (!found && _peers.size() < DISCOVERY_MAX_PEERS) {
    _peers.push_back(std::move(p));
  }
  xSemaphoreGive(_mutex);
}

void DiscoveryService::pruneStale(uint32_t now) {
  if (!_mutex) return;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  for (size_t i = 0; i < _peers.size();) {
    if (now - _peers[i].lastSeenMs > PEER_TIMEOUT_MS) {
      _peers[i] = std::move(_peers.back());
      _peers.pop_back();
    } else {
      ++i;
    }
  }
  xSemaphoreGive(_mutex);
}

void DiscoveryService::discoveryLoop() {
  char buf[MAX_PKT];
  for (;;) {
    // Wait for STA to actually have an IP before touching the socket.
    if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress((uint32_t)0)) {
      if (_udpReady) { _udp.stop(); _udpReady = false; }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (!_udpReady) {
      if (!openSocket()) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
      _udpReady = true;
      announceSelf();                 // announce immediately on (re)join
      _lastAnnounceMs = millis();
    }

    int sz = _udp.parsePacket();
    if (sz > 0) {
      int n = _udp.read((uint8_t*)buf, sizeof(buf) - 1);
      if (n > 0) processPacket(buf, n, _udp.remoteIP());
    } else {
      vTaskDelay(pdMS_TO_TICKS(RX_IDLE_DELAY_MS));
    }

    uint32_t now = millis();
    if (now - _lastAnnounceMs >= ANNOUNCE_INTERVAL_MS) {
      announceSelf();
      pruneStale(now);
      _lastAnnounceMs = now;
    }
  }
}

void DiscoveryService::servePeers(AsyncWebServerRequest* request) {
  // Measured payload: ~369 B for self + 0 peers; grows ~130 B per peer.
  // Start at 512 B; sendAdaptiveJson grows x2 under overflow and owns the
  // heap-pressure 503 — no hand-rolled guard needed here.
  sendAdaptiveJson(request, [this](JsonObject& root) {
    JsonObject self = root.createNestedObject("self");
    self["device_id"] = _canonical.length() ? _canonical : DeviceIdentity::canonical();
    self["project"]   = DeviceIdentity::projectName();
    self["fw"]        = DeviceIdentity::version();
    self["schema"]    = DISCOVERY_SCHEMA_VERSION;
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
  }, 512);
}
