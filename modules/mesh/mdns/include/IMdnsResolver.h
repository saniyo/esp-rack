#pragma once
#ifndef ESPRACK_IMDNS_RESOLVER_H
#define ESPRACK_IMDNS_RESOLVER_H

// IMdnsResolver — public contract for the MdnsService. Mirrors
// ICertProvider / IMqttProvider conventions: lives in this module's
// include/, App holds a pointer, late-bound by the owning Module's
// onInstall. Consumers (mothership client, cert manager, autoupdate)
// can call resolveHost("mothership.local") without depending on the
// concrete MdnsService class.
//
// Why even need this API when arduino-esp32's lwip auto-resolves
// "*.local" through MDNS.queryHost() in gethostbyname() once
// MDNS.begin() was called? Two reasons:
//   1. Explicit retry+timeout for the call sites that care about
//      latency budgets (mTLS handshake fast-paths).
//   2. Same surface for service browsing (peers / siblings) which
//      lwip doesn't help with — that's an MDNS.queryService().

#include <Arduino.h>
#include <IPAddress.h>
#include <vector>

class IMdnsResolver {
 public:
  virtual ~IMdnsResolver() = default;

  // Active = MDNS.begin() succeeded for the current WiFi session.
  // False until WiFi is up; flips false when WiFi drops and is
  // re-asserted after reconnect.
  virtual bool isActive() const = 0;

  // The label this device broadcasts itself as on the LAN, without
  // the ".local" suffix. Reachable from peers as `<hostname>.local`.
  virtual const char* hostname() const = 0;

  // Resolve a "*.local" host to its IPv4 via a single mDNS query.
  // Returns IPAddress(0,0,0,0) on timeout / not-found / not-active.
  // timeout_ms is per-call; first call may block up to that long.
  virtual IPAddress resolveHost(const String& host,
                                 uint32_t timeout_ms = 2000) = 0;

  struct ServiceEntry {
    String     instance;   // e.g. "mothership"
    IPAddress  ip;
    uint16_t   port;
    String     txt;        // joined "k=v" TXT records, debug-friendly
  };

  // Browse for instances of a given service type+proto on the LAN.
  // Returns a snapshot; the underlying mDNS cache continues to update
  // in the background. service = "_http" / "_https" / "_esprack"
  // (no leading underscore needed — implementation prepends if absent).
  // proto = "_tcp" or "_udp".
  virtual std::vector<ServiceEntry> browseService(
      const String& service,
      const String& proto,
      uint32_t timeout_ms = 2000) = 0;
};

#endif  // ESPRACK_IMDNS_RESOLVER_H
