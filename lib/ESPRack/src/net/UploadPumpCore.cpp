#include "net/UploadPumpCore.h"

#include "system/FlashSteps.h"

namespace ESPRack {

bool UploadPumpCore::begin(uint32_t holdBelow, uint32_t now) {
  if (!canBegin()) return false;
  // A ring nobody took back stays for this session.
  _phase = Phase::Running;
  _result = UploadResult::Ok;
  _open = false;
  _live = true;
  _finishing = false;
  _final = false;
  _held = false;
  _rd = 0;
  _used = 0;
  _written = 0;
  _holdBelow = holdBelow;
  _holds = 0;
  _progressAt = now;
  return true;
}

void UploadPumpCore::setRing(uint8_t* buf, uint32_t size) {
  _buf = size ? buf : nullptr;
  _size = _buf ? size : 0;
  _rd = 0;
  _used = 0;
}

uint8_t* UploadPumpCore::takeRing() {
  if (!_buf || _copying || _busy) return nullptr;
  if (_phase == Phase::Running && !failed()) return nullptr;
  uint8_t* b = _buf;
  _buf = nullptr;
  _size = 0;
  _rd = 0;
  _used = 0;
  return b;
}

void UploadPumpCore::final() {
  if (_phase == Phase::Running) _final = true;
}

void UploadPumpCore::fail(UploadResult r) {
  if (_phase != Phase::Running || r == UploadResult::Ok) return;
  if (!failed()) _result = r;
  if (!_live && !_busy) settle();  // nothing to abort
}

bool UploadPumpCore::reserve(uint32_t len, Span out[2]) {
  out[0] = Span{};
  out[1] = Span{};
  if (!accepting() || !_buf) return false;
  if (len > room()) {
    fail(UploadResult::Overflow);
    return false;
  }
  if (!len) return true;
  const uint32_t at = (_rd + _used) % _size;
  const uint32_t first = len < _size - at ? len : _size - at;
  out[0] = Span{_buf + at, first};
  if (len > first) out[1] = Span{_buf, len - first};
  _copying = true;
  return true;
}

void UploadPumpCore::commit(uint32_t len) {
  if (_copying && _phase == Phase::Running) _used += len;
  _copying = false;
}

UploadPumpCore::Window UploadPumpCore::window() {
  if (!accepting() || !_buf) {
    // Not kept (failed, all in, straight through): let the rest flow.
    if (_held) {
      _held = false;
      return Window::Reopen;
    }
    return Window::Open;
  }
  if (_held) {
    if (room() < _holdBelow) return Window::Hold;
    _held = false;
    return Window::Reopen;
  }
  if (room() < _holdBelow) {
    _held = true;
    ++_holds;
    return Window::Hold;
  }
  return Window::Open;
}

bool UploadPumpCore::pending() const {
  if (_phase != Phase::Running || _busy) return false;
  if (_finishing) return true;  // runs to its end, failed or not
  if (failed()) return _live;
  // A write waits for a whole step (up to the next 4 KB boundary) unless
  // all is in: one slot per sector. While the window is held there always
  // is one (the ring holds a step more than holdBelow).
  return !_open || _final || _used >= FlashSteps::room(_written);
}

UploadPumpCore::Action UploadPumpCore::next(Span out[2], uint32_t& len) {
  out[0] = Span{};
  out[1] = Span{};
  len = 0;
  if (!pending()) return Action::None;
  _busy = true;
  if (_finishing) return Action::Finish;
  if (failed()) return Action::Abort;
  if (!_open) return Action::Open;
  if (!_used) {  // all in, all written
    _finishing = true;
    return Action::Finish;
  }
  len = FlashSteps::step(_written, _used);
  const uint32_t first = len < _size - _rd ? len : _size - _rd;
  out[0] = Span{_buf + _rd, first};
  if (len > first) out[1] = Span{_buf, len - first};
  return Action::Write;
}

bool UploadPumpCore::claim() {
  if (_phase != Phase::Running || failed() || _busy || _copying || _used) return false;
  _busy = true;
  return true;
}

void UploadPumpCore::opened(bool ok) {
  if (ok) {
    _open = true;
  } else {
    fail(UploadResult::SinkFailed);
  }
}

void UploadPumpCore::wrote(uint32_t n, bool ok, bool fromRing) {
  if (!ok) {
    fail(UploadResult::SinkFailed);
    return;
  }
  _written += n;
  if (fromRing && _size) {
    if (n > _used) n = _used;
    _rd = (_rd + n) % _size;
    _used -= n;
  }
}

void UploadPumpCore::finished(int r) {
  if (r == UploadSink::kMore) return;  // the next call follows, whatever failed meanwhile
  _finishing = false;
  if (r == UploadSink::kDone) {
    // Committed: whatever happened meanwhile (a client that left during
    // the finish) there is nothing left to abort.
    _open = false;
    _live = false;
    settle();
    return;
  }
  fail(UploadResult::SinkFailed);
}

void UploadPumpCore::aborted() {
  _open = false;
  _live = false;
  _finishing = false;
  settle();
}

void UploadPumpCore::done() {
  _busy = false;
  if (_phase == Phase::Running && failed() && !_live) settle();
}

}  // namespace ESPRack
