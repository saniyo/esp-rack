#pragma once
#ifndef ESPRACK_DISCOVERY_SERVICE_H
#define ESPRACK_DISCOVERY_SERVICE_H

// DiscoveryService — discovers sibling ESP-Rack devices on the LAN over a
// custom UDP membership protocol (NOT mDNS, NOT a mesh network — see
// DiscoveryModule.h for the naming rationale).
//
// HOW (SWIM "C-subset": multicast self-announce + timeout liveness):
//   * Announce: every ~8 s each device multicasts a small UDP packet carrying
//     its identity + a project-key HMAC trust tag. This multicast is the
//     cold-join bootstrap — the one piece SWIM itself does not provide.
//   * Learn: a background task receives announces, recomputes the trust tag
//     from the shared project key (proving the peer is "ours" without ever
//     transmitting the key) + checks manifest-schema compatibility, and upserts
//     the peer into a FIXED BSS table. Peers that stop announcing age out.
//   * Serve: GET /api/v1/discovery/peers returns {self, peers[]}.
//
// MEMORY: the peer table is a fixed PeerRow[] in BSS (file-static in the .cpp,
// NOT a member — DiscoveryService is heap-allocated, so a member array would
// land on the heap). Fixed char[] fields, zero per-peer heap, zero churn. This
// is what keeps C3 max_alloc stable at 50+ peers — a std::vector<Peer> would
// grow into a ~8 KB contiguous HEAP block and crater max_alloc (see
// DISCOVERY_PROTOCOL.md §4 + the reserve(64) regression that proved it).
//
// Roadmap: full SWIM — unicast direct + k-indirect probing, incarnation gossip,
// per-packet HMAC + NVS epoch replay-hardening, anti-entropy /table + /digest.
//
// The table is read on the AsyncWebServer thread and written on the discovery
// task → guarded by a mutex held only for the quick upsert / serialise.

#include <Arduino.h>
#include <IPAddress.h>
#include <WiFiUdp.h>

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

  // Post-network: compute our identity + trust tag, cache our announce packet,
  // then spawn the discovery task (which opens the UDP socket once STA has an
  // IP). Self-contained — no mDNS / resolver dependency.
  void begin();

 private:
  void servePeers(AsyncWebServerRequest* request);
  bool openSocket();                                      // (re)join the multicast group
  void announceSelf();                                    // multicast our cached record
  void processPacket(char* buf, int len, IPAddress src);  // parse + verify + upsert
  void pruneStale(uint32_t now);                          // drop peers past the timeout
  static void taskTramp(void* arg);
  void discoveryLoop();

  WiFiUDP           _udp;
  bool              _udpReady{false};
  uint32_t          _lastAnnounceMs{0};

  String            _psk;        // shared project key (the "internal key")
  String            _canonical;  // our DeviceIdentity::canonical()
  String            _sig;        // HMAC(_psk, _canonical) trust tag, hex
  uint16_t          _webPort{80};

  // Announce packet built ONCE in begin() (identity is immutable) → zero
  // per-announce heap churn. Sized for the pipe-delimited record.
  char              _announceBuf[320];
  int               _announceLen{0};

  SemaphoreHandle_t _mutex{nullptr};   // guards the file-static peer table
  TaskHandle_t      _task{nullptr};
};

#endif  // ESPRACK_DISCOVERY_SERVICE_H
