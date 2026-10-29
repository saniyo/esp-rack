#pragma once
#ifndef PresenceLogQueue_h
#define PresenceLogQueue_h

// The log lines the flash write gate put off (power/WriteGate.h), in RAM
// until the FlashWriter job writes them. Bounded by lines and bytes: when
// full, the oldest go (counted). A batch is what one gate slot writes: the
// oldest lines up to maxBytes (at least one line). Not thread-safe: the
// caller locks. Str: Arduino String on the device, std::string in the host
// tests (both have length() and +=).

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <utility>

template <class Str>
class PresenceLogQueue {
 public:
  PresenceLogQueue(size_t maxLines, size_t maxBytes) : _maxLines(maxLines ? maxLines : 1), _maxBytes(maxBytes) {}

  // Adds a line; returns how many old lines it pushed out.
  size_t push(Str&& line) {
    _bytes += line.length();
    _q.push_back(std::move(line));
    size_t out = 0;
    // The newest line always stays, even one longer than maxBytes.
    while (_q.size() > 1 && (_q.size() > _maxLines || _bytes > _maxBytes)) {
      _bytes -= _q.front().length();
      _q.pop_front();
      ++out;
    }
    _dropped += uint32_t(out);
    return out;
  }

  // Moves the oldest lines, maxBytes in all at most (one line at least),
  // onto the end of out. Returns how many.
  size_t take(Str& out, size_t maxBytes) {
    size_t n = 0, bytes = 0;
    while (!_q.empty() && (n == 0 || bytes + _q.front().length() <= maxBytes)) {
      const size_t len = _q.front().length();
      out += _q.front();
      bytes += len;
      _bytes -= len;
      _q.pop_front();
      ++n;
    }
    return n;
  }

  // Lines lost on the way (a failed write): counted with the dropped ones.
  void lost(size_t lines) { _dropped += uint32_t(lines); }

  bool empty() const { return _q.empty(); }
  size_t lines() const { return _q.size(); }
  size_t bytes() const { return _bytes; }
  uint32_t dropped() const { return _dropped; }

 private:
  std::deque<Str> _q;
  size_t _maxLines;
  size_t _maxBytes;
  size_t _bytes = 0;
  uint32_t _dropped = 0;
};

#endif  // PresenceLogQueue_h
