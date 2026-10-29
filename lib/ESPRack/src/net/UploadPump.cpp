#include "net/UploadPump.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lwip/opt.h>
#include <string.h>

#include "power/FlashWriter.h"
#include "power/WriteGate.h"
#include "system/FlashSteps.h"

namespace ESPRack {

// lwIP's receive window and segment size of this build (sdkconfig:
// CONFIG_LWIP_TCP_WND_DEFAULT 5760, CONFIG_LWIP_TCP_MSS 1436 on every chip).
const uint32_t UploadPump::kHoldBelow = uint32_t(TCP_WND) + 2u * uint32_t(TCP_MSS);
const uint32_t UploadPump::kRingMin = FlashSteps::kSector + UploadPump::kHoldBelow;

static_assert(uint32_t(ESPRACK_UPLOAD_RING_PSRAM) >= FlashSteps::kSector + uint32_t(TCP_WND) + 2u * uint32_t(TCP_MSS),
              "ESPRACK_UPLOAD_RING_PSRAM must hold a step and a TCP window");

using Action = UploadPumpCore::Action;
using Window = UploadPumpCore::Window;
using Span = UploadPumpCore::Span;

bool UploadPump::attach(const char* name) {
  if (_job >= 0) return true;
  _job = FlashWriter::add(name, [this]() -> uint32_t { return run(); });
  if (_job < 0) log_e("[upload] %s: no FlashWriter job left", name ? name : "?");
  return _job >= 0;
}

// ---------------------------------------------------------------- async_tcp

bool UploadPump::begin(AsyncWebServerRequest* request, UploadSink sink) {
  if (!request || !request->client()) return false;
  if (_req) return false;  // an upload runs, or its client is still there
  if (_job < 0) driveFree();  // no job: the last upload's abort, if the gate lets it now
  {
    Lock l(_mux);
    if (!_core.canBegin()) return false;  // the last upload's sink is still being aborted
  }
  reclaim();
  // No session runs and the sink is idle: nobody calls _sink now. The
  // lock in _core.begin() publishes it to the FlashWriter task.
  _sink = std::move(sink);
  _req = request;
  _client = request->client();
  _resp = nullptr;
  _respond = nullptr;
  _closed = nullptr;
  _finishCalled = false;
  _watching = false;
  const uint32_t gen = ++_gen;
  request->onDisconnect([this, gen]() { gone(gen); });
  Lock l(_mux);
  (void)_core.begin(kHoldBelow, WriteGate::now());
  return true;
}

bool UploadPump::push(AsyncWebServerRequest* request, const uint8_t* data, size_t len, bool final) {
  if (!owns(request)) return false;
  const uint32_t now = WriteGate::now();
  bool keep;
  bool direct = false;
  {
    Lock l(_mux);
    keep = _core.accepting();
    if (keep) {
      if (len) _core.noteProgress(now);
      direct = len <= FlashSteps::kSector && _core.claim();  // one bounded write
    }
  }
  if (keep && direct) {
    // Straight through while the gate is free (and nothing waits in the
    // ring): as uploads always went.
    WritePass pass(now, true);
    if (pass.ok()) {
      bool open;
      {
        Lock l(_mux);
        open = _core.isOpen();
      }
      bool ok = true;
      if (!open) {
        ok = !_sink.open || _sink.open();
        Lock l(_mux);
        _core.opened(ok);
      }
      if (ok && len) {
        ok = _sink.write && _sink.write(data, len);
        Lock l(_mux);
        _core.wrote(static_cast<uint32_t>(len), ok, false);
      }
      {
        Lock l(_mux);
        if (final) _core.final();
        _core.noteProgress(WriteGate::now());
        _core.done();
      }
      pass.release();
      driveFree();  // finish, or the abort after a failure
      return ok;
    }
    Lock l(_mux);
    _core.done();  // not now: the ring takes it
  }
  bool kept = false;
  if (keep && len && _job < 0) {
    // Nothing would drain a ring: without a job only the free gate works.
    log_e("[upload] flash writes held and no FlashWriter job: upload failed");
    Lock l(_mux);
    _core.fail(UploadResult::NoWriter);
  } else if (keep && len && ensureRing()) {
    Span s[2];
    bool overflow;
    {
      Lock l(_mux);
      kept = _core.reserve(static_cast<uint32_t>(len), s);
      overflow = !kept && _core.result() == UploadResult::Overflow;
    }
    if (kept) {
      // Outside the lock: the writer only reads what is committed.
      memcpy(s[0].data, data, s[0].len);
      if (s[1].len) memcpy(s[1].data, data + s[0].len, s[1].len);
      Lock l(_mux);
      _core.commit(static_cast<uint32_t>(len));
    } else if (overflow) {
      log_e("[upload] ring overflow: upload failed");
    }
  } else if (keep && !len) {
    kept = true;
  }
  Window w;
  {
    Lock l(_mux);
    if (kept && final) _core.final();
    w = _core.window();
  }
  if (w == Window::Hold) {
    _client->ackLater();  // this segment; every one while held
    watch();
  } else if (w == Window::Reopen) {
    _client->ack(SIZE_MAX);  // all held ones (AsyncClient clamps to what it holds)
  }
  if (keep) driveFree();  // a whole step waiting: here if the gate is free again, else the job
  return kept;
}

bool UploadPump::finish(AsyncWebServerRequest* request, Respond respond, Closed closed) {
  if (!owns(request)) return false;
  _finishCalled = true;
  _respond = std::move(respond);
  _closed = std::move(closed);
  driveFree();
  if (respondIfReady()) return true;
  // The flash work goes on: no response now (and no rx timeout); the poll
  // answers once the result is known.
  (void)request->pause();
  watch();
  return true;
}

bool UploadPump::respondIfReady() {
  if (!_req || !_respond) return false;
  UploadResult r;
  {
    Lock l(_mux);
    if (!_core.settled() && !_core.failed()) return false;
    r = _core.result();
  }
  Respond fn = std::move(_respond);
  _respond = nullptr;
  AsyncWebServerResponse* resp = fn ? fn(_req, r) : nullptr;
  if (!resp) resp = _req->beginResponse(r == UploadResult::Ok ? 200 : 500);
  _resp = resp;
  _req->send(resp);  // a paused request sends it now, else when the handler returns
  return true;
}

void UploadPump::watch() {
  if (_watching || !_client) return;
  _watching = true;
  // Replaces the request's poll (a no-op until it has a response: see the
  // header); poll() does its part once the pump has answered.
  _client->onPoll(&UploadPump::pollThunk, this);
  _rxTimeout = _client->getRxTimeout();
  _client->setRxTimeout(0);  // the stall time instead: a held window brings no data
}

void UploadPump::pollThunk(void* arg, AsyncClient* c) { static_cast<UploadPump*>(arg)->poll(c); }

void UploadPump::poll(AsyncClient* c) {
  if (!_client || c != _client) return;
  bool stalled = false;
  Window w;
  {
    Lock l(_mux);
    if (_core.phase() == UploadPumpCore::Phase::Running && !_core.failed() &&
        _core.stalled(WriteGate::now(), ESPRACK_UPLOAD_STALL_MS)) {
      _core.fail(UploadResult::Stalled);
      stalled = true;
    }
    w = _core.window();
  }
  if (w == Window::Reopen) c->ack(SIZE_MAX);
  if (stalled) {
    log_w("[upload] no progress for %u ms: upload failed", (unsigned)ESPRACK_UPLOAD_STALL_MS);
    // A client still sending sends the rest (dropped) and gets the answer
    // from finish(); one that stopped is closed by its rx timeout again.
    if (!_finishCalled && _rxTimeout) c->setRxTimeout(_rxTimeout);
    driveFree();
    if (!_client) return;
  }
  if (_job < 0) {
    driveFree();  // no job: what waits for the gate goes from here once it is free
    if (!_client) return;
  }
  if (_respond) {
    (void)respondIfReady();
    return;
  }
  // After the answer: what the request's own poll did.
  if (_resp && c->canSend()) {
    if (!_resp->_finished()) {
      _resp->_ack(_req, 0, 0);
    } else {
      c->close();  // gone() runs from here; the request deletes the response
    }
  }
}

void UploadPump::gone(uint32_t gen) {
  if (gen != _gen || !_req) return;
  UploadResult r;
  {
    Lock l(_mux);
    _core.fail(UploadResult::Aborted);  // nothing once settled
    r = _core.result();
  }
  Closed cb = _finishCalled ? std::move(_closed) : Closed();
  _closed = nullptr;
  _respond = nullptr;
  _req = nullptr;
  _client = nullptr;
  _resp = nullptr;
  _watching = false;
  if (r == UploadResult::Aborted) log_w("[upload] client gone: upload aborted");
  driveFree();  // the abort, now while the gate is free, else the job
  if (cb) cb(r);
}

// ------------------------------------------------------------ either task

void UploadPump::act(Action a, Span s[2], uint32_t len) {
  switch (a) {
    case Action::Open: {
      const bool ok = !_sink.open || _sink.open();
      Lock l(_mux);
      _core.opened(ok);
      break;
    }
    case Action::Write: {
      bool ok = _sink.write && _sink.write(s[0].data, s[0].len);
      if (ok && s[1].len) ok = _sink.write(s[1].data, s[1].len);
      Lock l(_mux);
      _core.wrote(len, ok, true);
      break;
    }
    case Action::Finish: {
      const int r = _sink.finish ? _sink.finish() : UploadSink::kDone;
      Lock l(_mux);
      _core.finished(r);
      break;
    }
    case Action::Abort: {
      if (_sink.abort) _sink.abort();
      Lock l(_mux);
      _core.aborted();
      break;
    }
    case Action::None:
      break;
  }
  Lock l(_mux);
  _core.noteProgress(WriteGate::now());
  _core.done();
}

void UploadPump::driveFree() {
  // async_tcp: what is due runs here while the gate is free (a few steps
  // at most: async_tcp has its watchdog), the rest on the FlashWriter.
  constexpr uint8_t kMaxSteps = 4;
  bool pending;
  {
    Lock l(_mux);
    pending = _core.pending();
  }
  if (pending) {
    WritePass pass(WriteGate::now(), true);
    for (uint8_t i = 0; pass.ok() && i < kMaxSteps; ++i) {
      Span s[2];
      uint32_t len = 0;
      Action a;
      {
        Lock l(_mux);
        a = _core.next(s, len);
      }
      if (a == Action::None) break;
      act(a, s, len);
    }
  }
  reclaim();
  {
    Lock l(_mux);
    pending = _core.pending();
  }
  if (pending) FlashWriter::kick(_job);
}

uint32_t UploadPump::run() {
  Span s[2];
  uint32_t len = 0;
  Action a;
  {
    Lock l(_mux);
    a = _core.next(s, len);
  }
  if (a != Action::None) {
    WriteSlot slot;  // one bounded flash operation (a step of <= 4 KB)
    if (!slot.ok()) {
      Lock l(_mux);
      _core.done();
      return FlashWriter::kOnPower;  // the data stays in the ring
    }
    act(a, s, len);
  }
  reclaim();
  Lock l(_mux);
  return _core.pending() ? 1 : FlashWriter::kDone;  // 1 ms: the other jobs get their turn
}

bool UploadPump::ensureRing() {
  {
    Lock l(_mux);
    if (_core.hasRing()) return true;
  }
  uint32_t size = ESPRACK_UPLOAD_RING_PSRAM;
  uint8_t* b = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!b) {
    size = kRingMin;
    b = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (!b) {
    log_e("[upload] no %u B for the ring: upload failed", (unsigned)kRingMin);
    Lock l(_mux);
    _core.fail(UploadResult::NoMemory);
    return false;
  }
  Lock l(_mux);
  _core.setRing(b, size);
  return true;
}

void UploadPump::reclaim() {
  uint8_t* b;
  {
    Lock l(_mux);
    b = _core.takeRing();
  }
  if (b) heap_caps_free(b);
}

bool UploadPump::active() const {
  Lock l(_mux);
  return _core.phase() == UploadPumpCore::Phase::Running;
}

UploadResult UploadPump::result() const {
  Lock l(_mux);
  return _core.result();
}

uint32_t UploadPump::written() const {
  Lock l(_mux);
  return _core.written();
}

uint32_t UploadPump::holds() const {
  Lock l(_mux);
  return _core.holds();
}

}  // namespace ESPRack
