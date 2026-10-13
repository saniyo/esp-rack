#include <ESPRackPure/LimitMonitor.h>

namespace ESPRack {

LimitMonitor::Ev LimitMonitor::feed(float v, bool valid, uint64_t monoMs) {
  if (!valid || v != v) {  // invalid or NaN
    _pending = false;
    return Ev::None;
  }
  const bool towards = _tripped ? (_cfg.upper ? v < _cfg.limit - _cfg.hyst : v > _cfg.limit + _cfg.hyst)
                                : (_cfg.upper ? v > _cfg.limit : v < _cfg.limit);
  if (!towards) {
    _pending = false;
    return Ev::None;
  }
  if (!_pending) {
    _pending = true;
    _since = monoMs;
  }
  const uint64_t dwell = monoMs >= _since ? monoMs - _since : 0;
  if (dwell < (_tripped ? _cfg.clearMs : _cfg.tripMs)) return Ev::None;
  _pending = false;
  _tripped = !_tripped;
  return _tripped ? Ev::Tripped : Ev::Cleared;
}

}  // namespace ESPRack
