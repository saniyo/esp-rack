#include "PersistentTlsClient.h"
#include "ITLSProvider.h"

namespace ESPRack {

PersistentTlsClient::PersistentTlsClient() : _client(nullptr) {
  allocateClient();
}

void PersistentTlsClient::allocateClient() {
  // Heap-allocate so we can free + reallocate on hardReset(). The
  // ctor itself doesn't grab the 32 KB mbedtls record buffers — that
  // happens later on first connect(). What we DO get from construction
  // is the small wrapper state (~few hundred bytes).
  _client = new WiFiClientSecure();
  _connection_live = false;
  if (_tls) _tls->attachToClient(*_client);
  _client->setHandshakeTimeout(15);
  _client->setTimeout(15000);
}

void PersistentTlsClient::configure(ITLSProvider* tls) {
  _tls = tls;
  if (_client && _tls) {
    _tls->attachToClient(*_client);
  }
}

bool PersistentTlsClient::extractHostPort(const String& url,
                                            String& out_host,
                                            uint16_t& out_port) {
  // Minimal parser — we accept only `https://host[:port]/path`.
  // Anything else (http://, plain host) is a configuration error
  // that the caller should catch via the returned false.
  if (!url.startsWith("https://")) return false;
  int authority_start = 8;  // length of "https://"
  int slash = url.indexOf('/', authority_start);
  String authority = (slash > 0)
                     ? url.substring(authority_start, slash)
                     : url.substring(authority_start);
  int colon = authority.indexOf(':');
  if (colon > 0) {
    out_host = authority.substring(0, colon);
    out_port = (uint16_t)authority.substring(colon + 1).toInt();
    if (out_port == 0) return false;
  } else {
    out_host = authority;
    out_port = 443;
  }
  return out_host.length() > 0;
}

bool PersistentTlsClient::ensureHost(const String& url) {
  String host;
  uint16_t port = 0;
  if (!extractHostPort(url, host, port)) return false;
  if (host == _current_host && port == _current_port) {
    // Same host — nothing to do. Existing connection (if any) stays.
    return true;
  }
  // Host changed. Drop the existing TCP connection (mbedtls state
  // stays in the WiFiClientSecure since we don't recreate it). Next
  // beginRequest will reconnect to the new host using the same
  // structure.
  if (_client && _client->connected()) {
    _client->stop();
    // Note: WiFiClientSecure::stop frees the mbedtls SSL session.
    // We accept the realloc cost on host change because it's rare
    // (operator profile switch) and the alternative would be a
    // separate WiFiClientSecure per host, which doubles the peak
    // mbedtls footprint.
  }
  _current_host = host;
  _current_port = port;
  _connection_live = false;
  _stats.last_host = host;
  _stats.last_port = port;
  return true;
}

bool PersistentTlsClient::beginRequest(const String& url) {
  if (!ensureHost(url)) {
    log_e("[tls.persist] bad URL: %s", url.c_str());
    return false;
  }
  _stats.lifetime_requests++;

  if (_connection_live && _client && _client->connected()) {
    // Reuse path — the HTTPClient with setReuse(true) keeps the
    // socket open across end/begin cycles. mbedtls scratch buffers
    // stay allocated. ZERO new mbedtls allocations on this path.
    _stats.lifetime_reuse_hits++;
  } else {
    // Fresh connection — TLS handshake will happen during the first
    // POST. This is the ONE path where mbedtls allocates its 32 KB
    // record buffers. We expect this to fire once per boot per
    // host, not once per request.
    _stats.lifetime_handshakes++;
    log_i("[tls.persist] new TLS handshake — host=%s port=%u "
          "(handshake #%u for this client)",
          _current_host.c_str(),
          (unsigned)_current_port,
          (unsigned)_stats.lifetime_handshakes);
  }

  // HTTPClient setup. setReuse(true) is the load-bearing line.
  _http.setReuse(true);
  if (!_http.begin(*_client, url)) {
    log_e("[tls.persist] http.begin failed for %s", url.c_str());
    return false;
  }
  _http.setTimeout(20000);
  return true;
}

HTTPClient& PersistentTlsClient::http() {
  return _http;
}

void PersistentTlsClient::endRequest() {
  // HTTPClient::end with setReuse(true) preserves the underlying
  // socket — we should NOT see a mbedtls free here. Verified via
  // the lifetime_handshakes counter not incrementing on subsequent
  // beginRequest calls.
  _http.end();
  // _connection_live tracks our intent: we WANT the next request
  // to reuse. The actual liveness comes from _client->connected()
  // checked in beginRequest.
  _connection_live = _client && _client->connected();
}

void PersistentTlsClient::hardReset() {
  log_w("[tls.persist] hardReset — recreating WiFiClientSecure "
        "(handshakes so far: %u, reuse hits: %u)",
        (unsigned)_stats.lifetime_handshakes,
        (unsigned)_stats.lifetime_reuse_hits);
  _stats.lifetime_hard_resets++;
  if (_http.connected()) _http.end();
  if (_client) {
    _client->stop();
    delete _client;
    _client = nullptr;
  }
  _connection_live = false;
  allocateClient();
}

PersistentTlsClient::Stats PersistentTlsClient::stats() const {
  return _stats;
}

}  // namespace ESPRack
