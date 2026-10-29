#include <DiscoveryService.h>

#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include <errno.h>
#include <lwip/sockets.h>
#include "mbedtls/md.h"

#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <AdaptiveJson.h>
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

// Max peers the fixed BSS table holds. BSS cost = MAX_PEERS * sizeof(PeerRow)
// (~104 B) ≈ 6.5 KB at 64. Headroom over the "50+" target. It lives in BSS so
// it shrinks the heap REGION once at link time but NEVER fragments the heap
// (unlike a std::vector that grows into a multi-KB contiguous heap block) —
// so C3 max_alloc stays stable no matter how many peers show up.
#ifndef DISCOVERY_MAX_PEERS
#define DISCOVERY_MAX_PEERS 64
#endif

namespace {
const char*    PEERS_PATH            = "/api/v1/discovery/peers";
const char*    DISCOVERY_GROUP       = "239.42.7.1";   // org-local multicast group
const uint16_t DISCOVERY_PORT        = 7711;
const uint32_t ANNOUNCE_INTERVAL_MS  = 8000;    // self-announce cadence
const uint32_t PEER_TIMEOUT_MS       = 30000;   // ~3.75 missed announces -> drop
const uint32_t LINK_CHECK_MS         = 1000;    // longest wait: a dropped link is seen within it
const uint32_t PEER_LOCK_MS          = 100;     // peer table lock (held for microseconds)
const size_t   MAX_PKT               = 320;     // one announce fits comfortably
const char     PKT_MAGIC             = 'E';     // "E5|2|..." wire prefix
const uint8_t  PKT_VER               = 2;

// ── Fixed BSS peer table ─────────────────────────────────────────────────
// File-static => lives in .bss, NOT on the heap (DiscoveryService itself is
// new'd, so a member array would be heap-resident). Fixed char[] fields => no
// per-peer String alloc, no churn. Field widths are tight to the canonical ID
// layout (<project>-<mac12>-<uid8>) + the small version/schema strings.
struct PeerRow {
  uint32_t ip;            // IPv4 raw (host uint32 from IPAddress)
  uint32_t lastSeenMs;
  uint16_t port;
  bool     used;
  bool     trusted;       // project-key HMAC verified
  bool     compatible;    // manifest schema we can render
  char     schema[4];
  char     deviceId[40];  // canonical "<project>-<mac12>-<uid8>"
  char     project[20];
  char     fw[14];
  char     framework[12];
};
PeerRow s_peers[DISCOVERY_MAX_PEERS];   // .bss, zero-initialised

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

  // Build the announce packet ONCE — our identity is immutable, so there is no
  // reason to re-serialise (and re-allocate Strings) every 8 s.
  // Wire: "E5|2|<device_id>|<project>|<fw>|<schema>|<fw_fx>|<sig>|<port>".
  _announceLen = snprintf(_announceBuf, sizeof(_announceBuf),
                          "%c%u|%u|%s|%s|%s|%s|%s|%s|%u",
                          PKT_MAGIC, (unsigned)5, (unsigned)PKT_VER,
                          _canonical.c_str(),
                          DeviceIdentity::projectName().c_str(),
                          DeviceIdentity::version().c_str(),
                          DISCOVERY_SCHEMA_VERSION,
                          ESPRACK_VERSION_STR,
                          _sig.c_str(),
                          (unsigned)_webPort);
  if (_announceLen <= 0 || _announceLen >= (int)sizeof(_announceBuf)) {
    log_w("[discovery] announce payload truncated (%d) — peers may not see us",
          _announceLen);
  }

  // Persistent discovery task. Static stack — the buffer lives in BSS, not heap
  // (same P4 pattern as Mothership/Telegram). 4096 B: HW-measured high-water of
  // the WiFiUDP recv + parse path is ~2.4 KB (stack_free 1688), so 4096 keeps a
  // healthy ~1.7 KB margin. Re-measure stack_free in System->Resources if the
  // SWIM probing phase adds depth.
  // GOT_IP ends the task's no-link wait at once (its own notification; the
  // task is static and never deleted, so the handle stays valid).
  WiFi.onEvent([this](WiFiEvent_t, WiFiEventInfo_t) {
    if (_task) xTaskNotifyGive(_task);
  }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);

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

  log_i("[discovery] up id=%s sig=%s key=%s table=%uB(bss)", _canonical.c_str(),
        _sig.c_str(), (_psk == "esprack-discovery-dev") ? "DEFAULT(dev)" : "custom",
        (unsigned)sizeof(s_peers));
}

void DiscoveryService::taskTramp(void* arg) {
  static_cast<DiscoveryService*>(arg)->discoveryLoop();
}

namespace {
in_addr_t groupAddr() {
  IPAddress group;
  group.fromString(DISCOVERY_GROUP);
  return (in_addr_t)(uint32_t)group;
}
}  // namespace

bool DiscoveryService::openSocket() {
  // What WiFiUDP::beginMulticast does (bind the port on any address, join
  // the group on the default interface), on a socket of our own: WiFiUDP
  // keeps its descriptor private, and select() needs it.
  _sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (_sock < 0) {
    log_w("[discovery] socket() failed: %d", errno);
    return false;
  }
  int yes = 1;
  setsockopt(_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(DISCOVERY_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = groupAddr();
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  if (bind(_sock, (sockaddr*)&addr, sizeof(addr)) < 0 ||
      setsockopt(_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    log_w("[discovery] joining %s:%u failed: %d", DISCOVERY_GROUP, DISCOVERY_PORT, errno);
    close(_sock);
    _sock = -1;
    return false;
  }
  fcntl(_sock, F_SETFL, O_NONBLOCK);
  log_i("[discovery] listening on %s:%u", DISCOVERY_GROUP, DISCOVERY_PORT);
  return true;
}

void DiscoveryService::closeSocket() {
  if (_sock < 0) return;
  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = groupAddr();
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  setsockopt(_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
  close(_sock);
  _sock = -1;
}

void DiscoveryService::announceSelf() {
  if (_announceLen <= 0 || _sock < 0) return;
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(DISCOVERY_PORT);
  to.sin_addr.s_addr = groupAddr();
  if (sendto(_sock, _announceBuf, _announceLen, 0, (sockaddr*)&to, sizeof(to)) < 0) {
    log_d("[discovery] announce failed: %d", errno);
  }
}

void DiscoveryService::processPacket(char* buf, int len, IPAddress src) {
  if (len < 6 || len >= (int)MAX_PKT) return;
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
  if (f[0][0] != PKT_MAGIC || f[0][1] != '5') return;
  if ((uint8_t)atoi(f[1]) != PKT_VER) return;

  const char* deviceId = f[2];
  // Skip self and any unidentified responder.
  if (deviceId[0] == '\0' || _canonical == deviceId) return;

  const char* sig = f[7];
  // Same trust model as the mDNS-TXT version: recompute HMAC(PSK, device_id)
  // and compare to the advertised tag — proves shared-PSK ownership.
  bool trusted    = sig[0] && (hmacHex(_psk, String(deviceId), 16) == sig);
  bool compatible = (strcmp(f[5], DISCOVERY_SCHEMA_VERSION) == 0);
  uint32_t ipRaw  = (uint32_t)src;
  uint16_t port   = (uint16_t)atoi(f[8]);

  if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(PEER_LOCK_MS)) != pdTRUE) return;  // the next announce
  // Find existing row by deviceId, else first free slot.
  int slot = -1, freeSlot = -1;
  for (int i = 0; i < DISCOVERY_MAX_PEERS; ++i) {
    if (s_peers[i].used) {
      if (strcmp(s_peers[i].deviceId, deviceId) == 0) { slot = i; break; }
    } else if (freeSlot < 0) {
      freeSlot = i;
    }
  }
  if (slot < 0) slot = freeSlot;
  if (slot >= 0) {
    PeerRow& r = s_peers[slot];
    strlcpy(r.deviceId,  deviceId, sizeof(r.deviceId));
    strlcpy(r.project,   f[3],     sizeof(r.project));
    strlcpy(r.fw,        f[4],     sizeof(r.fw));
    strlcpy(r.schema,    f[5],     sizeof(r.schema));
    strlcpy(r.framework, f[6],     sizeof(r.framework));
    r.ip         = ipRaw;
    r.port       = port;
    r.trusted    = trusted;
    r.compatible = compatible;
    r.lastSeenMs = millis();
    r.used       = true;
  }
  // else: table full — drop (a 64-peer subnet is already past the design point).
  xSemaphoreGive(_mutex);
}

void DiscoveryService::pruneStale(uint32_t now) {
  if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(PEER_LOCK_MS)) != pdTRUE) return;  // the next round
  for (int i = 0; i < DISCOVERY_MAX_PEERS; ++i) {
    if (s_peers[i].used && (now - s_peers[i].lastSeenMs) > PEER_TIMEOUT_MS) {
      s_peers[i].used = false;
    }
  }
  xSemaphoreGive(_mutex);
}

void DiscoveryService::discoveryLoop() {
  char buf[MAX_PKT];
  for (;;) {
    // Wait for STA to actually have an IP before touching the socket.
    // Exception (own task, priority 1): the 1 s re-check stays, as a wait on
    // the task's notification that GOT_IP ends early.
    if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress((uint32_t)0)) {
      closeSocket();
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LINK_CHECK_MS));
      continue;
    }
    if (_sock < 0) {
      if (!openSocket()) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LINK_CHECK_MS)); continue; }
      announceSelf();                 // announce immediately on (re)join
      _lastAnnounceMs = millis();
    }

    // Block in select() until a packet lands or the next announce is due.
    uint32_t wait = millis() - _lastAnnounceMs;
    wait = wait >= ANNOUNCE_INTERVAL_MS ? 0 : ANNOUNCE_INTERVAL_MS - wait;
    if (wait > LINK_CHECK_MS) wait = LINK_CHECK_MS;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(_sock, &rd);
    timeval tv;
    tv.tv_sec = wait / 1000;
    tv.tv_usec = (wait % 1000) * 1000;
    const int ready = select(_sock + 1, &rd, nullptr, nullptr, &tv);
    if (ready < 0) {
      log_w("[discovery] select failed: %d, reopening", errno);
      closeSocket();
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LINK_CHECK_MS));
      continue;
    }
    if (ready > 0 && FD_ISSET(_sock, &rd)) {
      sockaddr_in src{};
      socklen_t slen = sizeof(src);
      const int n = recvfrom(_sock, buf, sizeof(buf) - 1, MSG_DONTWAIT, (sockaddr*)&src, &slen);
      if (n > 0) processPacket(buf, n, IPAddress((uint32_t)src.sin_addr.s_addr));
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
    // async_tcp: a bounded wait for the table; on a miss the list is left
    // out this once (the page asks again).
    if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(PEER_LOCK_MS)) != pdTRUE) {
      root["busy"] = true;
      return;
    }
    uint32_t now = millis();
    for (int i = 0; i < DISCOVERY_MAX_PEERS; ++i) {
      const PeerRow& p = s_peers[i];
      if (!p.used) continue;
      JsonObject o = arr.createNestedObject();
      o["device_id"]  = p.deviceId;
      o["project"]    = p.project;
      o["fw"]         = p.fw;
      o["schema"]     = p.schema;
      o["framework"]  = p.framework;
      o["ip"]         = IPAddress(p.ip).toString();
      o["port"]       = p.port;
      o["trusted"]    = p.trusted;
      o["compatible"] = p.compatible;
      o["age_ms"]     = (uint32_t)(now - p.lastSeenMs);
    }
    xSemaphoreGive(_mutex);
  }, 512);
}
