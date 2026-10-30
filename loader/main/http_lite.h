#pragma once
#ifndef ESPRACK_LOADER_HTTP_LITE_H
#define ESPRACK_LOADER_HTTP_LITE_H

// The loader's HTTP/1.1 download, the parts that need no sockets: the URL,
// the response head and chunked decoding. Plain http:// only: the image is
// signed, and esp_http_client would link the whole TLS stack (esp-tls,
// mbedtls' TLS and X.509: ~70 KB, more than the loader can spare).
//
// IDF and Arduino-free (host-tested: loader/test-host).

#include <stddef.h>
#include <stdint.h>

namespace loader {
namespace http {

struct Url {
  char host[64] = {};
  uint16_t port = 80;
  char path[256] = {};  // "/..." with the query, no fragment
};

// http://host[:port][/path][?query][#fragment]; false: not http:// (https
// included), or a part too long.
bool parseUrl(const char* url, Url& out);
// A redirect's Location against the URL it came from: absolute http://, or
// an absolute path on the same server. false: anything else.
bool redirect(const Url& from, const char* location, Url& out);

// The response head as it arrives.
class Head {
 public:
  static constexpr size_t kMax = 8192;  // a longer head is refused
  // Takes bytes up to the end of the head (the blank line) and returns
  // how many it took: the rest is body.
  size_t feed(const char* p, size_t n);
  bool done() const { return _done; }
  bool bad() const { return _bad; }  // not HTTP, or too long
  int status() const { return _status; }
  int64_t length() const { return _length; }  // Content-Length, -1 none
  bool chunked() const { return _chunked; }
  const char* location() const { return _location; }

 private:
  void line();
  char _line[300] = {};
  size_t _len = 0;
  size_t _total = 0;
  bool _first = true;
  bool _done = false;
  bool _bad = false;
  int _status = 0;
  int64_t _length = -1;
  bool _chunked = false;
  char _location[256] = {};
};

// Transfer-Encoding: chunked, decoded in place.
class Chunked {
 public:
  // The data bytes of p[0..n) into out (out may be p: it never runs
  // ahead); returns how many.
  size_t feed(const uint8_t* p, size_t n, uint8_t* out);
  bool end() const { return _state == State::End; }  // the last chunk and its trailer came
  bool bad() const { return _state == State::Bad; }

 private:
  enum class State : uint8_t { Size, Data, DataEnd, Trailer, End, Bad };
  State _state = State::Size;
  uint32_t _left = 0;
  uint8_t _digits = 0;
  bool _ext = false;
  uint16_t _lineLen = 0;
};

}  // namespace http
}  // namespace loader

#endif  // ESPRACK_LOADER_HTTP_LITE_H
