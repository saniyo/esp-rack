#pragma once
#ifndef ESPRACK_DISCOVERY_SERVICE_H
#define ESPRACK_DISCOVERY_SERVICE_H

// DiscoveryService — discovers sibling ESP-Rack devices on the LAN.
//
// This is LAN discovery (mDNS), not a mesh network — see DiscoveryModule.h for
// the naming rationale ("mesh" is reserved for a future device-to-device WiFi
// mesh).
//
// HOW:
//   * Advertise: rides the mDNS module's `_esprack._tcp` advert with three
//     extra TXT keys — `schema` (manifest envelope version we speak),
//     `fw_fx` (framework version), and `sig` (a project-key HMAC trust tag).
//   * Browse: a background task periodically queries `_esprack._tcp` (IDF
//     mdns API, which exposes TXT) and, for every responder, recomputes the
//     trust tag from the shared project key — proving the peer is "ours"
//     without ever transmitting the key — and checks manifest-schema
//     compatibility (so same-project AND cross-project devices we can render
//     are recognised). The verified, bounded peer list is cached.
//   * Serve: GET /api/v1/discovery/peers returns {self, peers[]} for the
//     Discovery page.
//
// The cache is read on the AsyncWebServer thread and written on the discovery
// task → guarded by a mutex held only for the quick swap/serialise (never
// during the blocking browse).

#include <Arduino.h>
#include <IPAddress.h>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <IMdnsResolver.h>

class WebManager;
class AsyncWebServerRequest;

class DiscoveryService {
 public:
  DiscoveryService() = default;

  // Registers the Discovery page (component "Discovery") + the
  // /api/v1/discovery/peers endpoint on the manifest. Called during onInstall.
  void registerManifest(WebManager* web);

  // Post-network: compute our trust tag, then spawn the discovery task.
  // `mdns` is the framework resolver (we only browse while it's active).
  void begin(IMdnsResolver* mdns);

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
  void ensureAdvert();       // (re)assert our extra TXT on the _esprack advert
  void browseAndUpdate();    // IDF mdns browse -> verify -> swap cache
  static void taskTramp(void* arg);
  void discoveryLoop();

  IMdnsResolver*    _mdns{nullptr};
  String            _psk;        // shared project key (the "internal key")
  String            _canonical;  // our DeviceIdentity::canonical()
  String            _sig;        // HMAC(_psk, _canonical) trust tag, hex
  std::vector<Peer> _peers;      // bounded, replaced each browse cycle
  SemaphoreHandle_t _mutex{nullptr};
  TaskHandle_t      _task{nullptr};
};

#endif  // ESPRACK_DISCOVERY_SERVICE_H
