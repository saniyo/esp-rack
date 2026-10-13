#include <ESPRackPure/CounterTracker.h>

namespace ESPRack {

void CounterTracker::restore(uint64_t anchor, uint16_t epoch, bool valid) {
  _anchor = anchor;
  _epoch = epoch;
  _valid = valid;
  _held = false;
  _heldCount = 0;
  _heldValue = 0;
  _heldMaxA = 0;
}

CounterStep CounterTracker::rebase(uint64_t v, int64_t credit, CounterEvent ev) {
  const CounterStep s{credit, ev, _anchor, v};
  _anchor = v;
  _valid = true;
  _held = false;
  _heldCount = 0;
  _heldValue = 0;
  _heldMaxA = 0;
  return s;
}

CounterStep CounterTracker::feed(uint64_t v, uint16_t epoch, int64_t maxA, int64_t maxH, bool evidence) {
  if (!_valid) {  // 1
    _epoch = epoch;
    return rebase(v, 0, CounterEvent::First);
  }
  if (epoch != _epoch) {  // 2
    _epoch = epoch;
    return rebase(v, static_cast<int64_t>(v) <= maxA ? static_cast<int64_t>(v) : 0, CounterEvent::ExpectedReset);
  }
  const int64_t a = static_cast<int64_t>(_anchor);
  const int64_t sv = static_cast<int64_t>(v);
  const int64_t dA = sv - a;
  if (!_held) {
    if (dA >= 0 && dA <= maxA) return rebase(v, dA, CounterEvent::Normal);                    // 3
    if (dA < 0 && dA >= -_cfg.negSlack && evidence) return rebase(v, 0, CounterEvent::NegSlack);  // 4
    if (_cfg.modulo > 0) {                                                                    // 5
      const int64_t mod = static_cast<int64_t>(_cfg.modulo);
      if (a >= mod - maxA && sv <= maxA && mod - a + sv <= maxA) return rebase(v, mod - a + sv, CounterEvent::Wrap);
    }
    _held = true;  // 6
    _heldCount = 1;
    _heldValue = v;
    _heldMaxA = maxA;
    return CounterStep{0, CounterEvent::Held, _anchor, _anchor};
  }
  if (dA >= 0 && dA <= maxA) return rebase(v, dA, CounterEvent::GlitchDropped);  // 7
  const int64_t h = static_cast<int64_t>(_heldValue);
  const int64_t dH = sv - h;
  if (dH >= 0 && dH <= maxH) {  // 8
    const bool reset = h < a;
    const int64_t credit = (reset && h <= _heldMaxA ? h : 0) + dH;
    return rebase(v, credit, reset ? CounterEvent::ResetConfirmed : CounterEvent::JumpConfirmed);
  }
  if (_heldCount + 1 < _cfg.maxHold) {  // 9
    ++_heldCount;
    _heldValue = v;
    _heldMaxA = maxA;
    return CounterStep{0, CounterEvent::Held, _anchor, _anchor};
  }
  return rebase(v, 0, CounterEvent::ForcedRebase);  // 10
}

}  // namespace ESPRack
