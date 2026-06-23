#pragma once
#ifndef ESPRACK_DISCOVERY_SERVICE_H
#define ESPRACK_DISCOVERY_SERVICE_H

// DiscoveryService — discovers sibling ESP-Rack devices on the LAN over a
// custom UDP membership protocol (NOT mDNS, NOT a mesh network — see
// DiscoveryModule.h for the naming rationale).
//
// HOW (v1 — the SWIM "C-subset": multicast self-announce + timeout liveness):
//   * Announce: every ~8 s each device multicasts a small UDP packet carrying
//     its identity + a project-key HMAC trust tag (the SAME trust model the old
//     mDNS-TXT version used). This multicast is the cold-join bootstrap — the
//     one piece SWIM itself does not provide.
//   * Learn: a background task receives announces, recomputes the trust tag
//     from the shared project key (proving the peer is "ours" without ever
//     transmitting the key) + checks manifest-schema compatibility, and upserts
//     the peer into a bounded table. Peers that stop announcing age out.
//   * Serve: GET /api/v1/discovery/peers returns {self, peers[]}.
//
// Roadmap (modules/discovery/DISCOVERY_PROTOCOL.md): layer on full SWIM —
// unicast probing (direct + k indirect) + incarnation gossip + per-packet
// HMAC/epoch replay-hardening + a fixed BSS table for true 50+ heap
// determinism. This v1 deliberately reuses the String-based Peer with UPSERT
// (no per-cycle rebuild like the old mDNS browse) to keep the change small and
// the trust/serve logic identical.
//
// The table is read on the AsyncWebServer thread and written on the discovery
// task → guarded by a mutex held only for the quick upsert / serialise.

#include <Arduino.h>
#include <IPAddress.h>
#include <WiFiUdp.h>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

class WebManager;
class AsyncWebServerRequest;

class DiscoveryService {
 public:
  DiscoveryService() = default;

  // Registers the Discovery page (component "Discovery") + the
  // /api/v1/discovery/peers endpoint on the manifest. Called during onInstall.
  void registerManifest(WebManager* web);

  // Post-network: compute our identity + trust tag, then spawn the discovery
  // task. The task opens the UDP socket once STA has an IP. Self-contained —
  // no mDNS / resolver dependency.
  void begin();

 private:
  struct Peer {
    String    deviceId;
    String    project;
    String    fw;
    String    schema;
    String    framework;
    String    sig;
    IPAddress ip;
    uint16_t  port{0};
    bool      trusted{false};     // project-key HMAC verified
    bool      compatible{false};  // manifest schema we can render
    uint32_t  lastSeenMs{0};
  };

  void servePeers(AsyncWebServerRequest* request);
  bool openSocket();                                   // (re)join the multicast group
  void announceSelf();                                 // multicast our record
  void processPacket(char* buf, int len, IPAddress src);  // parse + verify + upsert
  void upsertPeer(Peer&& p);                           // add or update in place
  void pruneStale(uint32_t now);                       // drop peers past the timeout
  static void taskTramp(void* arg);
  void discoveryLoop();

  WiFiUDP           _udp;
  bool              _udpReady{false};
  uint32_t          _lastAnnounceMs{0};

  String            _psk;        // shared project key (the "internal key")
  String            _canonical;  // our DeviceIdentity::canonical()
  String            _sig;        // HMAC(_psk, _canonical) trust tag, hex
  uint16_t          _webPort{80};
  std::vector<Peer> _peers;      // bounded; upserted in place (not rebuilt)
  SemaphoreHandle_t _mutex{nullptr};
  TaskHandle_t      _task{nullptr};
};

#endif  // ESPRACK_DISCOVERY_SERVICE_H
