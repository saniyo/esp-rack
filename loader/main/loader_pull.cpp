// esprack-loader: one download of the handoff's image, plain HTTP/1.1 on
// lwIP sockets (http_lite.h: why not esp_http_client). The token goes as
// "Authorization: Bearer <token>", the same on every attempt (it holds up
// to the install, brief §6). Everything the response tells before
// its body (status, length) and the image's first sector are checked
// before the first flash write. Redirects: up to 3, http:// only.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include "http_lite.h"
#include "image_writer.h"
#include "loader.h"

namespace loader {

namespace {

const char* const TAG = "loader";
constexpr uint32_t kConnectMs = 10000;
constexpr uint32_t kIoMs = 15000;  // per read / write
uint8_t s_rx[2048];  // the pull's (one at a time: Session::writer)

// A connected socket, or -1 (setError done).
int connectTo(const http::Url& u) {
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  char port[8];
  snprintf(port, sizeof(port), "%u", unsigned(u.port));
  addrinfo* res = nullptr;
  if (getaddrinfo(u.host, port, &hints, &res) != 0 || !res) {
    setError("download: %s not found", u.host);
    return -1;
  }
  const int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  bool ok = s >= 0;
  if (ok) {
    // Non-blocking connect, so a host that doesn't answer costs kConnectMs.
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    if (connect(s, res->ai_addr, res->ai_addrlen) != 0 && errno != EINPROGRESS) {
      ok = false;
    } else {
      fd_set w;
      FD_ZERO(&w);
      FD_SET(s, &w);
      timeval tv = {long(kConnectMs / 1000), 0};
      int err = 0;
      socklen_t len = sizeof(err);
      ok = select(s + 1, nullptr, &w, nullptr, &tv) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 &&
           err == 0;
    }
    fcntl(s, F_SETFL, flags);
    const timeval io = {long(kIoMs / 1000), 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &io, sizeof(io));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &io, sizeof(io));
  }
  freeaddrinfo(res);
  if (!ok) {
    setError("download: no connection to %s:%u", u.host, unsigned(u.port));
    if (s >= 0) close(s);
    return -1;
  }
  return s;
}

bool sendAll(int s, const char* p, size_t n) {
  while (n) {
    const int k = send(s, p, n, 0);
    if (k <= 0) return false;
    p += k;
    n -= size_t(k);
  }
  return true;
}

// Sends the request and reads the head; the body bytes that came with it
// are left at s_rx[0..*extra). -1: failed (setError done).
int request(const http::Url& u, const char* token, http::Head& head, size_t* extra) {
  const int s = connectTo(u);
  if (s < 0) return -1;
  static char req[sizeof(u.path) + sizeof(u.host) + 200 + sizeof(ota::Handoff::token)];
  const int n = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: esprack-loader\r\n%s%s%s"
                         "Connection: close\r\n\r\n",
                         u.path, u.host, unsigned(u.port), token[0] ? "Authorization: Bearer " : "", token,
                         token[0] ? "\r\n" : "");
  if (n <= 0 || size_t(n) >= sizeof(req) || !sendAll(s, req, size_t(n))) {
    setError("download: request not sent");
    close(s);
    return -1;
  }
  head = http::Head{};
  while (!head.done()) {
    const int k = recv(s, s_rx, sizeof(s_rx), 0);
    if (k <= 0) {
      setError("download: no answer");
      close(s);
      return -1;
    }
    const size_t used = head.feed(reinterpret_cast<const char*>(s_rx), size_t(k));
    if (head.bad()) {
      setError("download: not an HTTP answer");
      close(s);
      return -1;
    }
    if (head.done()) {
      *extra = size_t(k) - used;
      memmove(s_rx, s_rx + used, *extra);
    }
  }
  return s;
}

}  // namespace

Pull pullOnce() {
  const ota::Handoff& h = g.handoff;
  setPhase(Phase::Pulling);
  g.bytes.store(0);
  g.total.store(h.size);
  http::Url url;
  if (!http::parseUrl(h.url, url)) {
    setError("download: not an http:// URL");
    return Pull::Before;
  }
  http::Head head;
  size_t extra = 0;
  int s = -1;
  for (int hop = 0;; ++hop) {
    s = request(url, h.token, head, &extra);
    if (s < 0) return Pull::Before;
    const int st = head.status();
    if (st < 300 || st >= 400 || st == 304 || hop == 3) break;
    close(s);
    http::Url next;
    if (!http::redirect(url, head.location(), next)) {
      setError("download: redirect not followed (%s)", head.location());
      return Pull::Before;
    }
    url = next;
  }
  const int64_t len = head.length();
  const int64_t trailer = int64_t(ota::kTrailerSize);
  bool bad = true;
  if (head.status() != 200) {
    setError("download: HTTP %d", head.status());
  } else if (len > int64_t(g.slot->size) + trailer) {
    setError("download: image larger than the app slot");
  } else if (h.size && len >= 0 && len != int64_t(h.size) && len != int64_t(h.size) + trailer) {
    setError("download: %u B, %u announced", unsigned(len), unsigned(h.size));
  } else {
    bad = false;
  }
  if (bad) {
    close(s);
    return Pull::Before;
  }
  if (len >= 0) g.total.store(uint32_t(len));
  ImageWriter& w = imageWriter();
  ImageWriter::Expect x;
  x.size = h.size;
  x.sha256 = ota::hasSha256(h) ? h.sha256 : nullptr;
  x.sig = h.sigLen ? h.sig : nullptr;
  x.sigLen = h.sigLen;
  w.begin(g.slot, x);
  http::Chunked chunks;
  const bool chunked = head.chunked();
  uint32_t got = 0;  // body bytes (decoded)
  size_t n = extra;  // s_rx holds n body bytes
  bool ok = true;
  for (;;) {
    if (chunked) n = chunks.feed(s_rx, n, s_rx);
    else if (len >= 0 && got + n > uint64_t(len)) n = size_t(uint64_t(len) - got);
    if (n) {
      got += uint32_t(n);
      g.bytes.store(got);
      if (!w.feed(s_rx, n)) {
        setError("%s", w.error());
        ok = false;
        break;
      }
    }
    if (chunked ? chunks.end() : (len >= 0 && got >= uint64_t(len))) break;
    if (chunked && chunks.bad()) {
      setError("download: bad chunked encoding");
      ok = false;
      break;
    }
    const int k = recv(s, s_rx, sizeof(s_rx), 0);
    if (k < 0) {
      setError("download: read failed at %u B", unsigned(got));
      ok = false;
      break;
    }
    if (k == 0) {  // closed: the end only for a body without a length
      if (chunked || len >= 0) {
        setError("download: cut off at %u B", unsigned(got));
        ok = false;
      }
      break;
    }
    n = size_t(k);
  }
  close(s);
  if (ok && !w.finish()) {
    setError("%s", w.error());
    ok = false;
  }
  if (!ok) {
    const bool touched = w.touched();
    w.abandon();
    if (touched) g.appValid = false;
    return touched ? Pull::After : Pull::Before;
  }
  g.appValid = false;  // ota_0 now holds the new image, not the old app
  g.imageIn.store(true);  // under Session::writer: no upload over it
  ota::setText(g.newVersion, w.version());
  if (w.warning()[0]) setWarning("%s", w.warning());
  ESP_LOGI(TAG, "downloaded %s: %u B", g.newVersion, unsigned(got));
  return Pull::Ok;
}

}  // namespace loader
