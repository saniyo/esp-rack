#pragma once
#ifndef PersistentTlsClient_h
#define PersistentTlsClient_h

// PersistentTlsClient — Phase 1 of the memory-fragmentation master
// plan. BearSSL via ESP_SSLClient (mobizt), with our own minimal
// HTTP/1.1 client on top because arduino-esp32 v3.x's HTTPClient
// requires a NetworkClient subclass, not the bare `Client&` that
// ESP_SSLClient exposes.
//
// Why we left mbedtls behind
// --------------------------
// WiFiClientSecure / mbedtls allocates ~32 KB of in/out content
// buffers on every handshake. ESP-IDF's prebuilt mbedtls has
// `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=1` baked in, so a runtime
// `mbedtls_platform_set_calloc_free()` is a no-op — every reconnect
// goes through the system heap, fragments accumulate, and after a
// few cycles `MBEDTLS_ERR_SSL_ALLOC_FAILED (-32512)` kills
// handshakes for the rest of the boot. Soak data and the full
// post-mortem live in `docs/plans/memory-fragmentation-master-plan.md`.
//
// BearSSL takes caller-supplied static buffers and does NOT malloc
// during handshake. Buffer sizes are pinned via the
// `SSLCLIENT_HALF_DUPLEX` / `STATIC_IN_BUFFER_SIZE` /
// `STATIC_OUT_BUFFER_SIZE` defines in platformio.ini, so each
// translation unit that includes `<ESP_SSLClient.h>` picks them up
// automatically. The whole TLS layer becomes BSS-resident, immune
// to fragmentation.
//
// USAGE
// -----
//   class MyService {
//     ESPRack::PersistentTlsClient _tls;
//     ...
//   };
//
//   _tls.configure(_app->tls());            // attach CA+cert (one shot)
//
//   String resp;
//   int code = _tls.post(url, body, "application/json", resp);
//   if (code < 200 || code >= 300) { ... }
//   // ...consume resp...
//
// The `post()` call reuses the existing TCP+TLS session when it
// can. The server has to send back `Connection: keep-alive` (HTTP/1.1
// default); a `Connection: close` from the peer drops the socket
// and the next call will re-handshake. Stats (`lifetime_handshakes`
// vs `lifetime_reuse_hits`) make the reuse rate observable.

#include <Arduino.h>
#include <WiFiClient.h>
#include <ESP_SSLClient.h>

class ITLSProvider;

namespace ESPRack {

class PersistentTlsClient {
 public:
  PersistentTlsClient();

  // Attach the framework TLS provider so the underlying client gets
  // the CA chain + device cert/key. Safe to call multiple times —
  // each call replays the current provider's view. Cert load is lazy
  // on the FIRST post() if the provider wasn't ready yet.
  void configure(ITLSProvider* tls);

  // Bootstrap / insecure mode — no CA pinned, no client cert. BearSSL
  // skips peer-cert chain validation AND the NTP-clock gate (the clock
  // is only needed to validate cert NotBefore/NotAfter, which we're not
  // doing here). The channel is still TLS-encrypted; only peer
  // authentication is waived. Used by cert-manager's enroll / recover
  // flows, where the device has no trust anchor yet (the CA bundle
  // arrives in the very response). Mutually exclusive with configure().
  void configureInsecure();

  // POST `body` as `content_type` to the given HTTPS URL. Returns
  // the HTTP status code on success, or a negative transport error:
  //   -1 — bad URL / DNS / TCP-connect / TLS-handshake failure
  //   -2 — request write failure (socket dropped mid-send)
  //   -3 — response read timeout / empty response
  //   -4 — malformed HTTP status line
  //   -5 — body truncated before Content-Length
  //
  // On success the response body is placed in `out_response` (cleared
  // first; size capped to `kMaxResponseBytes` to keep memory bounded).
  // On any failure the connection is dropped (so the next call
  // re-handshakes); on success the connection is kept alive for reuse
  // unless the server sent `Connection: close`.
  // `extra_headers`, when non-null, is injected verbatim into the
  // request header block — the caller supplies complete "Name: value\r\n"
  // line(s) (e.g. an "Authorization: Bearer …" line for enroll). Pass
  // nullptr for none; existing callers are unaffected.
  int post(const String& url,
           const String& body,
           const char*   content_type,
           String&       out_response,
           const char*   extra_headers = nullptr);

  // Hard reset — drop the current socket + clear the connection-live
  // flag. With BearSSL this does NOT free the static in/out buffers
  // (they live in BSS), so recovery is bounded and predictable.
  // Used after consecutive transport failures to dislodge a stuck
  // TCP state.
  void hardReset();

  struct Stats {
    uint32_t lifetime_requests;
    uint32_t lifetime_handshakes;   // first connect after hardReset / new host
    uint32_t lifetime_hard_resets;
    uint32_t lifetime_reuse_hits;   // request that reused an existing connection
    String   last_host;
    uint16_t last_port;
  };
  Stats stats() const;

  // Cap on response body retained in `out_response` so a misbehaving
  // server can't blow up our heap. Mothership responses are typically
  // 30–500 B; 4 KB is two orders of magnitude headroom while staying
  // well under any plausible heap-pressure threshold.
  static constexpr size_t kMaxResponseBytes = 4096;

  // Override the response-body retention cap (default kMaxResponseBytes).
  // cert-manager bumps this so a cert + full CA-chain PEM response isn't
  // truncated. Call before post().
  void setMaxResponseBytes(size_t n) { _max_response_bytes = n; }

 private:
  WiFiClient    _tcp;
  ESP_SSLClient _ssl;
  ITLSProvider* _tls{nullptr};
  String        _current_host;
  uint16_t      _current_port{0};
  String        _current_path;
  bool          _connection_live{false};
  bool          _certs_loaded{false};
  // Bootstrap/insecure mode (see configureInsecure). When true, certs
  // are not loaded and the NTP gate + peer validation are skipped.
  bool          _insecure{false};
  // Response-body retention cap; overridable per-instance via
  // setMaxResponseBytes (default kMaxResponseBytes).
  size_t        _max_response_bytes{kMaxResponseBytes};
  // Stable BSS-resident copy of the host string handed to
  // ESP_SSLClient::connectSSL(). Empirically, passing
  // _current_host.c_str() (heap-resident String body) into the
  // library produced DNS lookups with garbage bytes — looked like
  // overrun from a nearby log printf buffer. A member char[] sits in
  // BSS, has a stable address for the object's lifetime, and is
  // immune to that aliasing.
  char          _host_static[64]{};

  Stats         _stats{};

  void loadCerts();
  bool extractHostPortPath(const String& url, String& host,
                           uint16_t& port, String& path);
  bool ensureConnected();
  // Read one CRLF-terminated line up to `max_len` bytes, with a
  // deadline (ms since boot). Returns the line WITHOUT the CRLF.
  // Empty string means timeout or socket close.
  String readLine(uint32_t deadline_ms, size_t max_len);
};

}  // namespace ESPRack

#endif  // PersistentTlsClient_h
