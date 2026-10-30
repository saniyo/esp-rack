#include "http_lite.h"

#include <stdlib.h>
#include <string.h>

namespace loader {
namespace http {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

// s starts with prefix, ignoring case.
bool startsWith(const char* s, const char* prefix) {
  for (; *prefix; ++s, ++prefix)
    if (lower(*s) != lower(*prefix)) return false;
  return true;
}

int hexVal(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// An absolute path ("/..." or "?..."), the fragment dropped.
bool setPath(const char* p, Url& out) {
  const size_t n = strcspn(p, "#");
  const bool slash = *p == '/';
  if (n + (slash ? 0 : 1) >= sizeof(out.path)) return false;
  size_t o = 0;
  if (!slash) out.path[o++] = '/';
  memcpy(out.path + o, p, n);
  out.path[o + n] = 0;
  return true;
}

}  // namespace

bool parseUrl(const char* url, Url& out) {
  if (!url || !startsWith(url, "http://")) return false;
  const char* h = url + 7;
  const size_t hn = strcspn(h, ":/?#");
  if (!hn || hn >= sizeof(out.host)) return false;
  memcpy(out.host, h, hn);
  out.host[hn] = 0;
  const char* p = h + hn;
  out.port = 80;
  if (*p == ':') {
    uint32_t port = 0;
    const char* d = ++p;
    while (*p >= '0' && *p <= '9' && port <= 65535) port = port * 10 + uint32_t(*p++ - '0');
    if (p == d || port == 0 || port > 65535) return false;
    out.port = uint16_t(port);
  }
  if (*p == '/' || *p == '?') return setPath(p, out);
  if (*p && *p != '#') return false;
  out.path[0] = '/';
  out.path[1] = 0;
  return true;
}

bool redirect(const Url& from, const char* location, Url& out) {
  if (startsWith(location, "http://")) return parseUrl(location, out);
  if (location[0] != '/' || location[1] == '/') return false;  // "//host/...": not followed
  out = from;
  return setPath(location, out);
}

size_t Head::feed(const char* p, size_t n) {
  size_t i = 0;
  while (i < n && !_done && !_bad) {
    const char c = p[i++];
    if (++_total > kMax) {
      _bad = true;
      break;
    }
    if (c == '\n') {
      if (_len && _line[_len - 1] == '\r') --_len;
      _line[_len] = 0;
      line();
      _len = 0;
    } else if (_len + 1 < sizeof(_line)) {
      _line[_len++] = c;  // a longer line is cut (a cookie, say): its name still counts
    }
  }
  return i;
}

void Head::line() {
  if (_first) {
    _first = false;
    // "HTTP/1.1 200 OK"
    if (!startsWith(_line, "HTTP/1.") || _len < 12 || _line[8] != ' ') {
      _bad = true;
      return;
    }
    _status = atoi(_line + 9);
    if (_status < 100 || _status > 599) _bad = true;
    return;
  }
  if (!_len) {
    _done = true;
    return;
  }
  const char* colon = strchr(_line, ':');
  if (!colon) return;
  const char* v = colon + 1;
  while (*v == ' ' || *v == '\t') ++v;
  if (startsWith(_line, "content-length:")) {
    char* end = nullptr;
    const long long n = strtoll(v, &end, 10);
    _length = end != v && n >= 0 ? int64_t(n) : -1;
  } else if (startsWith(_line, "transfer-encoding:")) {
    for (const char* s = v; *s; ++s)
      if (startsWith(s, "chunked")) _chunked = true;
  } else if (startsWith(_line, "location:")) {
    const size_t n = strlen(v) < sizeof(_location) - 1 ? strlen(v) : sizeof(_location) - 1;
    memcpy(_location, v, n);
    _location[n] = 0;
  }
}

size_t Chunked::feed(const uint8_t* p, size_t n, uint8_t* out) {
  size_t o = 0;
  size_t i = 0;
  while (i < n && _state != State::End && _state != State::Bad) {
    const uint8_t c = p[i];
    switch (_state) {
      case State::Size: {
        const int v = hexVal(c);
        if (v >= 0 && !_ext) {
          if (_left > 0x0FFFFFFFu) _state = State::Bad;
          _left = _left << 4 | uint32_t(v);
          ++_digits;
        } else if (c == ';') {
          _ext = true;
        } else if (c == '\n') {
          if (!_digits) {
            _state = State::Bad;
            break;
          }
          _state = _left ? State::Data : State::Trailer;
          _digits = 0;
          _ext = false;
          _lineLen = 0;
        } else if (!_ext && c != '\r' && c != ' ' && c != '\t') {
          _state = State::Bad;
        }
        ++i;
        break;
      }
      case State::Data: {
        const size_t k = n - i < _left ? n - i : size_t(_left);
        memmove(out + o, p + i, k);
        o += k;
        i += k;
        _left -= uint32_t(k);
        if (!_left) _state = State::DataEnd;
        break;
      }
      case State::DataEnd:
        if (c == '\n') _state = State::Size;
        else if (c != '\r') _state = State::Bad;
        ++i;
        break;
      case State::Trailer:
        if (c == '\n') {
          if (!_lineLen) _state = State::End;
          _lineLen = 0;
        } else if (c != '\r') {
          ++_lineLen;
        }
        ++i;
        break;
      default:
        break;
    }
  }
  return o;
}

}  // namespace http
}  // namespace loader
