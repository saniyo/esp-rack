#include "PersistentTlsClient.h"
#include "ITLSProvider.h"

#include <time.h>

namespace ESPRack {

PersistentTlsClient::PersistentTlsClient() {
  // BearSSL wrapper rides on top of a TCP `Client*`. WiFiClient is
  // the standard TCP socket from arduino-esp32. Attaching here means
  // _ssl is usable as soon as the object is constructed.
  _ssl.setClient(&_tcp);
  _ssl.setTimeout(15000);
  // Pin TLSv1.2 only. The mship Python server is configured with
  // `ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2`
  // and silently closes ClientHellos that announce a different
  // version preference — which matches the "send ClientHello, wait
  // forever for ServerHello" pattern we were seeing. Forcing the
  // version on BOTH ends removes the negotiation ambiguity.
  _ssl.setSSLVersion(0x0303, 0x0303);  // BR_TLS12 == 0x0303
  // Temporary: max-verbosity BearSSL debug so handshake failures get
  // explained on the serial wire. Level 4 = dump.
  _ssl.setDebugLevel(4);
}

void PersistentTlsClient::loadCerts() {
  if (_insecure) {
    // Bootstrap flows have no trust anchor and no client identity.
    _ssl.setInsecure();
    _certs_loaded = true;
    return;
  }
  if (!_tls) return;
  const String& ca = _tls->caBundlePem();
  const String& cert = _tls->clientCertPem();
  const String& key  = _tls->clientKeyPem();
  log_i("[tls.persist] loadCerts: ca=%u cert=%u key=%u",
        (unsigned)ca.length(),
        (unsigned)cert.length(),
        (unsigned)key.length());
  // CA is loaded so the chain CAN be validated when we connect by
  // hostname. For IP-literal hosts (the common intranet case for
  // mothership) BearSSL's minimal x509 context refuses to match an
  // IP against DNSName SAN entries — it ONLY understands DNS names
  // — and returns BR_ERR_X509_BAD_SERVER_NAME (56). The right
  // long-term fix is to connect via mDNS hostname (mothership.local
  // IS in the cert SAN), but until that's wired we call setInsecure()
  // when the host is an IP literal in ensureConnected(). TLS
  // encryption + mTLS client-cert auth still apply — only the
  // server-cert chain validation is skipped on the IP path.
  if (ca.length() > 0) {
    _ssl.setCACert(ca.c_str());
  }
  if (cert.length() > 0 && key.length() > 0) {
    _ssl.setCertificate(cert.c_str());
    // CRITICAL for ECDSA client certs: setPrivateKey() routes the key
    // to BearSSL's EC path but leaves `_allowed_usages = 0`, which
    // tells BearSSL "this key is not usable for ECDHE-ECDSA signing".
    // BearSSL then sends an empty CertificateVerify in the mTLS
    // handshake, the server (Python ssl.SSLContext) stalls waiting,
    // and the device sees a 60s hang in RECVREC followed by silent
    // close. The EC-aware setter wires in the correct flags:
    //   BR_KEYTYPE_SIGN  — needed for ECDHE-ECDSA signing
    //   BR_KEYTYPE_EC    — our CA is itself ECDSA-P256, so the
    //                      issuer key type is EC
    // Reference: esp8266/Arduino#8084, mobizt/ESP_SSLClient#23.
    _ssl.setPrivateECKey(key.c_str(),
                         BR_KEYTYPE_SIGN,
                         BR_KEYTYPE_EC);
  }
  _certs_loaded = (ca.length() > 0);
}

void PersistentTlsClient::configure(ITLSProvider* tls) {
  _insecure = false;
  _tls = tls;
  loadCerts();
}

void PersistentTlsClient::configureInsecure() {
  // No trust anchor, no client identity — BearSSL skips peer-cert
  // validation. Intended for the rare bootstrap flows (enroll /
  // recover) that run on an on-demand, heap-allocated client which is
  // freed the moment the flow completes, so this ~8 KB TLS footprint
  // is NOT resident in steady state.
  _insecure = true;
  _tls = nullptr;
  _certs_loaded = true;   // nothing to load
  _ssl.setInsecure();
}

bool PersistentTlsClient::extractHostPortPath(const String& url,
                                              String& host,
                                              uint16_t& port,
                                              String& path) {
  if (!url.startsWith("https://")) return false;
  int authority_start = 8;
  int slash = url.indexOf('/', authority_start);
  String authority = (slash > 0)
                     ? url.substring(authority_start, slash)
                     : url.substring(authority_start);
  path = (slash > 0) ? url.substring(slash) : String("/");
  int colon = authority.indexOf(':');
  if (colon > 0) {
    host = authority.substring(0, colon);
    port = (uint16_t)authority.substring(colon + 1).toInt();
    if (port == 0) return false;
  } else {
    host = authority;
    port = 443;
  }
  return host.length() > 0;
}

bool PersistentTlsClient::ensureConnected() {
  if (_connection_live && _ssl.connected()) {
    _stats.lifetime_reuse_hits++;
    return true;
  }
  // Fresh handshake. Lazy cert load if configure() ran before the
  // provider had certs available.
  if (!_certs_loaded && _tls && _tls->hasCaChain()) {
    loadCerts();
  }
  // BearSSL validates server cert NotBefore/NotAfter against an
  // explicit clock. Without setX509Time(), it falls back to its
  // compiled-in ESP_SSLCLIENT_VALID_TIMESTAMP (2023-08-02). Our
  // server cert NotBefore is in 2026, so the cert gets rejected as
  // "not yet valid" and the peer drops the handshake with ECONNRESET.
  // Gate the handshake on a sane wall-clock so the first checkin
  // waits for NTP (CertManager's task already waits on Wi-Fi+NTP
  // for the same reason).
  time_t now = time(nullptr);
  // The clock only matters for validating the server cert's
  // NotBefore/NotAfter. In insecure mode (no peer validation) skip the
  // gate so bootstrap enroll can run before SNTP has landed.
  if (!_insecure) {
    if (now < 1700000000) {  // 2023-11-14
      log_w("[tls.persist] skipping handshake — wall clock not yet "
            "synced (epoch=%lu, need NTP)", (unsigned long)now);
      return false;
    }
    _ssl.setX509Time((uint32_t)now);
  }
  _stats.lifetime_handshakes++;
  log_i("[tls.persist] new TLS handshake (BearSSL) — host=%s port=%u "
        "(handshake #%u for this client) X509Time=%lu",
        _current_host.c_str(),
        (unsigned)_current_port,
        (unsigned)_stats.lifetime_handshakes,
        (unsigned long)now);
  // Connect sequence has TWO steps because of how ESP_SSLClient is
  // layered. The library auto-upgrades connect() to TLS only on
  // well-known ports (see _secure_ports[26] in ESP_SSLClient.h —
  // 443, 465, 853, 993, 995, 8883, etc). Our mothership runs on
  // 8443, NOT in that list, so connect() falls through to a plain
  // TCP open. The convenience `connectSSL(host, port)` wrapper at
  // TCPClient.h:338 IGNORES the host/port arguments entirely and
  // dispatches to the no-arg connectSSL() which uses the internal
  // _host/_port — those are zero-initialised until connect() runs,
  // which is why our earlier "connectSSL(host,port)" call produced
  // a DNS lookup with garbage bytes (uninit _host buffer).
  //
  // The correct dance: connect() first (sets _host + opens TCP),
  // then connectSSL() no-args (runs the TLS handshake on the open
  // socket using the stored _host for SNI).
  //
  // We copy the host into a member char[] (BSS-resident, stable
  // address) before passing to connect(), so the strcpy inside the
  // library has a pointer that cannot move from under it.
  // BearSSL's minimal x509 validator matches the connection target
  // against DNSName SAN entries ONLY — it has no path to validate an
  // IP literal against an IPAddress SAN entry, and rejects with
  // BR_ERR_X509_BAD_SERVER_NAME (56). We tried two workarounds and
  // both failed:
  //   * setInsecure() wipes the client cert+key we need for mTLS
  //     (its mClearAuthenticationSettings() resets every slot), and
  //     subsequent setCACert/setCertificate calls re-clear the
  //     insecure flag too — no usable middle ground.
  //   * Rewriting target to "mothership.local" + relying on mDNS:
  //     arduino-esp32's hostByName has no mDNS responder running by
  //     default, so the lookup fails with -54 and the TCP connect
  //     never happens; also, the first failed lookup consumed ~150 KB
  //     of heap, which is unacceptable on the C3.
  // Resolution: keep the IP literal as-is on the device side, and
  // teach the SERVER to publish the IP as an IP-shaped *DNSName* SAN
  // entry in addition to the IPAddress entry. BearSSL minimal's
  // dn-matching loop happily accepts "192.168.10.202" as a DNSName
  // string and matches it against the IP literal we pass in. Not
  // RFC-strict, but transparent on the wire and unblocks the C3.
  size_t hl = _current_host.length();
  if (hl >= sizeof(_host_static)) hl = sizeof(_host_static) - 1;
  memcpy(_host_static, _current_host.c_str(), hl);
  _host_static[hl] = '\0';
  if (!_ssl.connect(_host_static, _current_port)) {
    log_e("[tls.persist] TCP connect failed to %s:%u",
          _host_static, (unsigned)_current_port);
    _connection_live = false;
    return false;
  }
  if (!_ssl.connectSSL()) {
    // Now SAFE to call getLastSSLError — the SSL engine reached
    // initialised state (TCP+SSL handshake started), so its error
    // context is valid. With ENABLE_ERROR_STRING in the build, this
    // returns a human-readable reason for the closure.
    char err_buf[200] = {0};
    int err_code = _ssl.getLastSSLError(err_buf, sizeof(err_buf));
    log_e("[tls.persist] TLS upgrade failed to %s:%u — BR err=%d (%s)",
          _host_static, (unsigned)_current_port,
          err_code, err_buf);
    _connection_live = false;
    return false;
  }
  _connection_live = true;
  return true;
}

String PersistentTlsClient::readLine(uint32_t deadline_ms,
                                     size_t max_len) {
  String out;
  out.reserve(max_len);
  while (millis() < deadline_ms) {
    int avail = _ssl.available();
    if (avail <= 0) {
      if (!_ssl.connected() && _ssl.available() <= 0) {
        // Socket dropped and buffer drained.
        return out;
      }
      delay(1);
      continue;
    }
    int c = _ssl.read();
    if (c < 0) {
      delay(1);
      continue;
    }
    if (c == '\n') {
      // Strip trailing \r if present.
      if (out.length() > 0 && out.charAt(out.length() - 1) == '\r') {
        out.remove(out.length() - 1);
      }
      return out;
    }
    if (out.length() >= max_len) {
      // Header longer than we'll tolerate — bail out as if EOL.
      return out;
    }
    out.concat((char)c);
  }
  // Timed out.
  return out;
}

int PersistentTlsClient::post(const String& url,
                              const String& body,
                              const char*   content_type,
                              String&       out_response,
                              const char*   extra_headers) {
  out_response = "";

  String new_host, new_path;
  uint16_t new_port = 0;
  if (!extractHostPortPath(url, new_host, new_port, new_path)) {
    log_e("[tls.persist] bad URL: %s", url.c_str());
    return -1;
  }

  // Host change drops any existing connection so the next
  // ensureConnected() does a clean fresh handshake to the new host.
  if (new_host != _current_host || new_port != _current_port) {
    if (_ssl.connected()) _ssl.stop();
    _tcp.stop();
    _connection_live = false;
    _current_host = new_host;
    _current_port = new_port;
    _stats.last_host = new_host;
    _stats.last_port = new_port;
  }
  _current_path = new_path;

  _stats.lifetime_requests++;

  if (!ensureConnected()) {
    return -1;
  }

  // Build the HTTP/1.1 request. Keep-Alive is the default; we send
  // the header explicitly so a wire trace shows our intent and so a
  // peer that ONLY looks at the literal header still keeps the
  // socket open. User-Agent matches what HTTPClient used to send so
  // any server-side log greps keep working.
  size_t extra_len = (extra_headers ? strlen(extra_headers) : 0);
  String req;
  req.reserve(256 + body.length() + extra_len);
  req += "POST ";       req += _current_path;        req += " HTTP/1.1\r\n";
  req += "Host: ";      req += _current_host;
  if (_current_port != 443) {
    req += ":"; req += String(_current_port);
  }
  req += "\r\n";
  req += "User-Agent: ESPRack-PersistentTlsClient\r\n";
  req += "Accept: */*\r\n";
  req += "Connection: keep-alive\r\n";
  if (extra_len) {
    req += extra_headers;   // caller supplies complete "Name: value\r\n" line(s)
  }
  req += "Content-Type: "; req += content_type;       req += "\r\n";
  req += "Content-Length: "; req += String(body.length()); req += "\r\n";
  req += "\r\n";
  req += body;

  size_t wrote = _ssl.write(
      reinterpret_cast<const uint8_t*>(req.c_str()), req.length());
  if (wrote != req.length()) {
    log_e("[tls.persist] short write: %u/%u",
          (unsigned)wrote, (unsigned)req.length());
    _ssl.stop();
    _tcp.stop();
    _connection_live = false;
    return -2;
  }

  // Status line: "HTTP/1.1 200 OK"
  uint32_t deadline = millis() + 20000;
  String status_line = readLine(deadline, 128);
  if (status_line.length() == 0) {
    log_w("[tls.persist] empty / timed-out response (peer reset?)");
    _ssl.stop();
    _tcp.stop();
    _connection_live = false;
    return -3;
  }
  int sp1 = status_line.indexOf(' ');
  int sp2 = status_line.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) {
    log_e("[tls.persist] bad status line: %s", status_line.c_str());
    _ssl.stop();
    _tcp.stop();
    _connection_live = false;
    return -4;
  }
  int status = status_line.substring(sp1 + 1, sp2).toInt();

  // Headers — we care about Content-Length and Connection: close.
  // Everything else is read past and discarded.
  long content_length = -1;
  bool close_after = false;
  while (millis() < deadline) {
    String header = readLine(deadline, 512);
    if (header.length() == 0) break;  // end of headers
    int colon = header.indexOf(':');
    if (colon <= 0) continue;
    String name  = header.substring(0, colon);
    String value = header.substring(colon + 1);
    value.trim();
    name.toLowerCase();
    if (name == "content-length") {
      content_length = value.toInt();
    } else if (name == "connection") {
      String v = value; v.toLowerCase();
      if (v.indexOf("close") >= 0) close_after = true;
    }
  }

  // Body — bounded by Content-Length and by our own cap. If the
  // server sent a > _max_response_bytes response, we still consume
  // the rest from the socket (so the next request finds a clean
  // stream) but only retain the first _max_response_bytes in
  // out_response. mship responses are tiny; this branch is purely
  // defensive.
  if (content_length > 0) {
    size_t retain_max = (size_t)content_length < _max_response_bytes
                        ? (size_t)content_length : _max_response_bytes;
    out_response.reserve(retain_max);
    long remaining = content_length;
    uint8_t buf[256];
    while (remaining > 0 && millis() < deadline) {
      int to_read = remaining < (long)sizeof(buf)
                    ? (int)remaining : (int)sizeof(buf);
      int got = _ssl.read(buf, to_read);
      if (got <= 0) {
        if (!_ssl.connected() && _ssl.available() <= 0) break;
        delay(1);
        continue;
      }
      if (out_response.length() < _max_response_bytes) {
        size_t space = _max_response_bytes - out_response.length();
        size_t take = (size_t)got < space ? (size_t)got : space;
        out_response.concat((const char*)buf, take);
      }
      remaining -= got;
    }
    if (remaining > 0) {
      log_w("[tls.persist] body short by %ld bytes", remaining);
      _ssl.stop();
      _tcp.stop();
      _connection_live = false;
      return -5;
    }
  }

  if (close_after) {
    _ssl.stop();
    _tcp.stop();
    _connection_live = false;
  } else {
    _connection_live = _ssl.connected();
  }

  return status;
}

void PersistentTlsClient::hardReset() {
  log_w("[tls.persist] hardReset (BearSSL) — stop+clear "
        "(handshakes so far: %u, reuse hits: %u)",
        (unsigned)_stats.lifetime_handshakes,
        (unsigned)_stats.lifetime_reuse_hits);
  _stats.lifetime_hard_resets++;
  _ssl.stop();
  _tcp.stop();
  _connection_live = false;
  // BearSSL static buffers stay; we just re-apply cert material in
  // case the user code rotated certs under us via updateClientCert.
  loadCerts();
}

PersistentTlsClient::Stats PersistentTlsClient::stats() const {
  return _stats;
}

}  // namespace ESPRack
