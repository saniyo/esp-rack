#include "power/WriteGateCore.h"

#include "power/PowerGuard.h"

namespace ESPRack {

uint32_t WriteGateCore::clamp(uint32_t now) {
  if (_seen && int32_t(now - _lastNow) < 0) return _lastNow;
  _seen = true;
  _lastNow = now;
  return now;
}

int8_t WriteGateCore::addHolder(const char* name) {
  if (_count >= kMaxHolders) return -1;
  _h[_count].name = name ? name : "?";
  return static_cast<int8_t>(_count++);
}

const char* WriteGateCore::holderName(int8_t h) const {
  return (h >= 0 && h < static_cast<int8_t>(_count)) ? _h[h].name : "";
}

bool WriteGateCore::holderOpen(int8_t h) const {
  return h >= 0 && h < static_cast<int8_t>(_count) && _h[h].open;
}

void WriteGateCore::open(int8_t h, uint32_t now) {
  if (h < 0 || h >= static_cast<int8_t>(_count)) return;
  now = clamp(now);
  windowOpen(now);  // a window past its limit ends before a new one starts
  Holder& x = _h[h];
  if (x.open) return;
  x.open = true;
  x.openedAt = now;
  x.forecast = false;
  ++_stats.windows;
  _used = true;
  _pause = false;
  if (_writing) startOverlap(now);
}

void WriteGateCore::close(int8_t h, uint32_t now) {
  now = clamp(now);
  closeAt(h, now, false, 0);
}

void WriteGateCore::close(int8_t h, uint32_t now, int32_t pauseMs) {
  now = clamp(now);
  closeAt(h, now, true, now + uint32_t(pauseMs > 0 ? pauseMs : 0));
}

void WriteGateCore::closeAt(int8_t h, uint32_t now, bool forecast, uint32_t nextAt) {
  if (h < 0 || h >= static_cast<int8_t>(_count)) return;
  windowOpen(now);
  Holder& x = _h[h];
  if (!x.open) return;  // ended by its limit already
  x.open = false;
  x.forecast = forecast;
  x.nextAt = nextAt;
  for (const Holder& o : _h) {
    if (o.open) return;  // another holder still needs the CPU: no pause yet
  }
  endWindow(now);
}

void WriteGateCore::endWindow(uint32_t at) {
  _lastEnd = at;
  _pause = true;
  _pauseTaken = false;
  endOverlap(at);
}

void WriteGateCore::startOverlap(uint32_t now) {
  if (_overlapOn) return;
  _overlapOn = true;
  _overlapAt = now;
  ++_stats.overlaps;
}

void WriteGateCore::endOverlap(uint32_t now) {
  if (!_overlapOn) return;
  _overlapOn = false;
  const uint32_t d = now - _overlapAt;
  if (int32_t(d) > 0 && d > _stats.longestOverlapMs) _stats.longestOverlapMs = d;
}

bool WriteGateCore::windowOpen(uint32_t now) {
  now = clamp(now);
  bool any = false;
  bool ended = false;
  uint32_t endAt = 0;
  for (Holder& x : _h) {
    if (!x.open) continue;
    if (now - x.openedAt >= _cfg.maxWindowMs) {
      x.open = false;
      x.forecast = false;
      ++_stats.expired;
      const uint32_t at = x.openedAt + _cfg.maxWindowMs;
      if (!ended || int32_t(at - endAt) > 0) endAt = at;
      ended = true;
    } else {
      any = true;
    }
  }
  if (ended && !any) endWindow(endAt);
  return any;
}

uint8_t WriteGateCore::openWindows(uint32_t now) {
  windowOpen(now);
  uint8_t n = 0;
  for (const Holder& x : _h) n += x.open ? 1 : 0;
  return n;
}

bool WriteGateCore::isFree(uint32_t now) {
  now = clamp(now);
  if (windowOpen(now) || !idle(now)) return false;
  bool any = false;
  const int32_t left = pauseLeft(now, any);
  return !any || left >= int32_t(_cfg.maxWindowMs);
}

uint32_t WriteGateCore::untilExpiry(uint32_t now) const {
  uint32_t left = _cfg.maxWindowMs;
  for (const Holder& x : _h) {
    if (!x.open) continue;
    const uint32_t age = now - x.openedAt;
    left = lesser(left, age < _cfg.maxWindowMs ? _cfg.maxWindowMs - age : 0);
  }
  return left;
}

int32_t WriteGateCore::pauseLeft(uint32_t now, bool& any) const {
  any = false;
  int32_t left = INT32_MAX;
  for (const Holder& x : _h) {
    if (x.open || !x.forecast) continue;
    const int32_t l = int32_t(x.nextAt - now);
    // A holder idleMs past its own forecast without a window has gone
    // quiet: its forecast stops counting.
    if (l <= -int32_t(_cfg.idleMs)) continue;
    any = true;
    if (l < left) left = l;
  }
  return left;
}

WriteGateCore::Ticket WriteGateCore::join(uint32_t) {
  if (_qLen >= kMaxQueue) return 0;
  Ticket t = _nextTicket++;
  if (!t) t = _nextTicket++;  // 0 is "none"
  _queue[_qLen++] = t;
  return t;
}

void WriteGateCore::quit(Ticket t) {
  if (!t) return;
  for (uint8_t i = 0; i < _qLen; ++i) {
    if (_queue[i] != t) continue;
    for (uint8_t j = i + 1; j < _qLen; ++j) _queue[j - 1] = _queue[j];
    --_qLen;
    return;
  }
}

bool WriteGateCore::headOf(Ticket t) const { return _qLen == 0 || (t && _queue[0] == t); }

WriteGateCore::Decision WriteGateCore::grant(uint32_t now, uint32_t since, bool forced, bool inPause,
                                             bool inWindow, bool commit) {
  if (commit) {
    ++_writing;
    ++_stats.grants;
    if (inPause) ++_stats.inPause;
    if (forced) ++_stats.forced;
    if (inPause || forced) {
      _gatedOn = true;
      _gatedAt = now;
    }
    if (inWindow) startOverlap(now);
    const uint32_t waited = now - since;
    if (int32_t(waited) > 0 && waited > _stats.longestWaitMs) _stats.longestWaitMs = waited;
  }
  return {Verdict::Go, 0, now};
}

WriteGateCore::Decision WriteGateCore::decide(uint32_t now, uint32_t since, bool onlyFree, Ticket t, bool commit) {
  now = clamp(now);
  if (!PowerGuard::writesAllowed()) {
    if (commit) ++_stats.refused;
    return {Verdict::Refused, 0};
  }
  const uint32_t waited = int32_t(now - since) > 0 ? now - since : 0;
  const uint32_t giveUp = _cfg.maxWaitMs + _cfg.maxWindowMs;  // no usable pause that long: go anyway
  const bool first = headOf(t);

  if (windowOpen(now)) {
    if (!onlyFree && first && !_writing && waited >= giveUp) return grant(now, since, true, false, true, commit);
    uint32_t retry = untilExpiry(now);
    if (!onlyFree && waited < giveUp) retry = lesser(retry, giveUp - waited);
    return {Verdict::Wait, retry ? retry : 1};
  }

  bool any = false;
  const int32_t left = pauseLeft(now, any);
  const bool quiet = idle(now);
  // Until a forecast that limits stops counting (idleMs past it).
  const uint32_t untilStale = any ? uint32_t(left + int32_t(_cfg.idleMs)) + 1 : 1;

  // Free: no windows lately (or ever), and none forecast within a whole
  // window's time: as without the gate, several at once. A forecast window
  // nearer than that makes this a pause: one writer (below).
  const bool freeNow = quiet && (!any || left >= int32_t(_cfg.maxWindowMs));
  // Once a holder has opened windows, one write at a time even while free:
  // a window may come back before the first is done. Woken when it leaves.
  if (freeNow && !(_used && _writing)) return grant(now, since, false, false, false, commit);
  if (freeNow) return {Verdict::Wait, _cfg.maxWindowMs};
  if (onlyFree) {
    uint32_t retry = !quiet ? _cfg.idleMs - (now - _lastEnd) : untilStale;
    if (quiet && left > 0) retry = lesser(retry, uint32_t(left));
    return {Verdict::Wait, retry ? retry : 1};
  }

  // Between windows: one writer, the first in the queue, if the earliest
  // forecast window is still minPauseMs away (a holder without a forecast
  // doesn't limit it); after maxWaitMs any pause with time left will do.
  if ((_pause || quiet) && !_pauseTaken && !_writing && first) {
    const bool roomy = !any || left >= int32_t(_cfg.minPauseMs);
    const bool due = waited >= _cfg.maxWaitMs && (!any || left > 0);
    if (roomy || due || waited >= giveUp) {
      if (commit && _pause) _pauseTaken = true;
      return grant(now, since, !roomy, true, false, commit);
    }
  }

  uint32_t retry = quiet ? untilStale : _cfg.idleMs - (now - _lastEnd);
  if (waited < _cfg.maxWaitMs) retry = lesser(retry, _cfg.maxWaitMs - waited);
  else if (waited < giveUp) retry = lesser(retry, giveUp - waited);
  return {Verdict::Wait, retry ? retry : 1};
}

WriteGateCore::Decision WriteGateCore::enter(uint32_t now, uint32_t since, bool onlyFree, Ticket t) {
  return decide(now, since, onlyFree, t, true);
}

bool WriteGateCore::wouldGo(uint32_t now, uint32_t since, bool onlyFree, Ticket t) {
  return decide(now, since, onlyFree, t, false).verdict == Verdict::Go;
}

WriteGateCore::Decision WriteGateCore::force(uint32_t now, uint32_t since) {
  now = clamp(now);
  if (!PowerGuard::writesAllowed()) {
    ++_stats.refused;
    return {Verdict::Refused, 0};
  }
  const bool inWindow = windowOpen(now);
  if (_pause) _pauseTaken = true;  // it uses this pause up
  return grant(now, since, true, false, inWindow, true);
}

void WriteGateCore::leave(uint32_t now, uint32_t heldSince) {
  now = clamp(now);
  if (!_writing) return;
  --_writing;
  const uint32_t held = int32_t(now - heldSince) > 0 ? now - heldSince : 0;
  if (_gatedOn && heldSince == _gatedAt) {
    _gatedOn = false;
    if (held > _stats.longestHoldMs) _stats.longestHoldMs = held;
  } else if (held > _stats.longestFreeHoldMs) {
    _stats.longestFreeHoldMs = held;
  }
  if (!_writing) endOverlap(now);
}

}  // namespace ESPRack
