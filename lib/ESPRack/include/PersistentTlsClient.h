#pragma once
#ifndef PersistentTlsClient_h
#define PersistentTlsClient_h

// PersistentTlsClient — Phase 1 of
// docs/plans/memory-fragmentation-master-plan.md.
//
// Pairs ONE long-lived `WiFiClientSecure` with ONE `HTTPClient`
// configured for HTTP/1.1 keep-alive reuse. The mbedtls SSL context
// owned by the WiFiClientSecure is created on first connect and
// HELD across requests — never freed during the device's lifetime.
//
// Why this matters: every WiFiClientSecure construction-then-stop
// cycle allocates ~32 KB for record buffers, then frees it. ESP-IDF's
// multi_heap allocator coalesces adjacent free blocks on free, but
// it does NOT compact. After many cycles, fragmentation accumulates
// and the 32 KB contiguous allocation begins to fail
// (MBEDTLS_ERR_SSL_ALLOC_FAILED -32512). On ESP32-C3 this happens in
// minutes; on ESP32-S3 in days; eventually on every target.
//
// Holding the structures forever sidesteps the fragmentation cycle
// entirely: the 32 KB lives once in the heap, the system can
// fragment around it however it wants, and TLS handshakes never run
// out of contiguous space.
//
// USAGE
// -----
//   // In a service that needs HTTPS:
//   class MyService {
//     ESPRack::PersistentTlsClient _tls;
//     ...
//   };
//
//   // First-time setup, after WiFi is up and the cert/CA is loaded:
//   _tls.configure(_app->tls());           // attach CA+cert
//
//   // Per-request:
//   if (!_tls.beginRequest(url)) { return false; }
//   HTTPClient& http = _tls.http();
//   http.addHeader("Content-Type", "application/json");
//   int code = http.POST(body);
//   String resp = http.getString();
//   _tls.endRequest();                     // PRESERVES TCP+TLS state
//
// The `endRequest()` call uses HTTPClient's reuse flag to keep the
// TCP socket + mbedtls state alive. Next `beginRequest` to the same
// host reuses everything. A different host (rare, e.g. cert-manager
// PKI override) triggers a clean disconnect+reconnect; the mbedtls
// SSL context is rebuilt on the new socket but the underlying
// WiFiClientSecure object isn't destroyed, so its CA chain stays.
//
// Server-side prerequisite: the peer must respect HTTP/1.1
// keep-alive (no `Connection: close` in its responses). The
// mothership device-API needs `BaseHTTPRequestHandler.protocol_version
// = "HTTP/1.1"` set; see Phase 1 of the master plan.

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

class ITLSProvider;

namespace ESPRack {

class PersistentTlsClient {
 public:
  PersistentTlsClient();

  // Attach the framework TLS provider so the underlying client gets
  // the CA chain + device cert/key. Safe to call multiple times —
  // each call replays the current provider's view.
  void configure(ITLSProvider* tls);

  // Optional: pre-bake the URL host so the first call to
  // beginRequest with that URL can fast-path. Called automatically
  // by beginRequest if not done explicitly.
  bool ensureHost(const String& url);

  // Prepare HTTPClient for a request. Returns false on host/port
  // change failure or HTTPClient.begin() failure. On success the
  // caller does addHeader / POST / etc, then calls endRequest().
  // setReuse(true) is applied unconditionally — the whole point.
  bool beginRequest(const String& url);

  // Read-only access to the wrapped HTTPClient for per-request
  // header / body work between begin and end.
  HTTPClient& http();

  // Finish the request WITHOUT freeing the underlying client.
  // HTTPClient::end() — with setReuse(true) and a peer that honours
  // Connection: keep-alive — leaves the TCP socket + mbedtls state
  // alive for the next call.
  void endRequest();

  // Hard reset — destroys + recreates the WiFiClientSecure. Use
  // ONLY when the connection has gone permanently bad (e.g. peer
  // changed its TLS cert, or we hit too many consecutive transport
  // errors). Throws away the mbedtls heap allocations and reallocates
  // on the next beginRequest, which is the very fragmentation-
  // surface this class exists to AVOID — so call only as a last
  // resort. Lifetime stats track how often this fires; a healthy
  // device should see 0 over its life.
  void hardReset();

  struct Stats {
    uint32_t lifetime_requests;
    uint32_t lifetime_handshakes;   // increments on first connect after a hardReset / new host
    uint32_t lifetime_hard_resets;
    uint32_t lifetime_reuse_hits;   // request that reused an existing connection
    String   last_host;
    uint16_t last_port;
  };
  Stats stats() const;

 private:
  // Heap-allocated so we can hardReset() by destroying + recreating.
  // Construction itself doesn't allocate the big mbedtls buffers —
  // those come on first connect().
  WiFiClientSecure* _client;
  HTTPClient        _http;
  ITLSProvider*     _tls{nullptr};
  String            _current_host;
  uint16_t          _current_port{0};
  bool              _connection_live{false};

  Stats             _stats{};

  void allocateClient();
  bool extractHostPort(const String& url, String& out_host, uint16_t& out_port);
};

}  // namespace ESPRack

#endif  // PersistentTlsClient_h
