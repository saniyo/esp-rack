#pragma once
#ifndef ESPRACK_UPLOAD_PUMP_CORE_H
#define ESPRACK_UPLOAD_PUMP_CORE_H

// UploadPumpCore: the bookkeeping of UploadPump (net/UploadPump.h) without
// locks, tasks or TCP:
//   - a ring the producer (async_tcp) fills and the writer drains in steps
//     (system/FlashSteps.h); one producer, one writer, each copying only
//     its own part outside the caller's lock (reserve/commit, next/wrote);
//   - when the producer holds the TCP window (less than holdBelow free)
//     and when it may open it again;
//   - the session's sink calls in order: open, the writes, finish (as many
//     calls as the sink asks for), or abort after a failure; one at a time
//     (busy from next()/claim() to done()). A failed session always ends
//     with the abort, opened or not (the sink may hold something from
//     before open). Once the first finish call ran, the finish runs to
//     its end whatever fails meanwhile (a sink between two of its finish
//     calls is in no state to abort); the failure stays the result.
// The caller serialises the calls. Clock: ms, wrapping. Arduino-free
// (host-tested).

#include <stdint.h>

#include "net/UploadSink.h"

namespace ESPRack {

enum class UploadResult : uint8_t {
  Ok = 0,
  SinkFailed,  // open, a write or finish said no
  Overflow,    // more came than the ring had room for
  NoMemory,    // no ring to be had
  NoWriter,    // a step had to wait for the gate, and the pump has no FlashWriter job
  Stalled,     // no progress for the stall time (writes held, a client that stopped)
  Aborted,     // the client went before the end
};

class UploadPumpCore {
 public:
  struct Span {
    uint8_t* data = nullptr;
    uint32_t len = 0;
  };
  enum class Action : uint8_t { None, Open, Write, Finish, Abort };
  enum class Window : uint8_t { Open, Hold, Reopen };
  enum class Phase : uint8_t { Idle, Running, Settled };

  // ---- session ----
  // A new session (Idle or Settled, sink not in use). holdBelow: free ring
  // bytes under which the producer holds the TCP window. false: busy.
  bool begin(uint32_t holdBelow, uint32_t now);
  bool canBegin() const { return _phase != Phase::Running && !_busy && !_copying; }
  // The session's ring (none: the producer writes straight through only).
  void setRing(uint8_t* buf, uint32_t size);
  bool hasRing() const { return _buf != nullptr; }
  // The ring once nobody can use it any more (settled or failed, no copy,
  // no step under way): the caller frees it. nullptr otherwise.
  uint8_t* takeRing();
  void final();                // the producer has pushed its last byte
  void fail(UploadResult r);   // the first failure counts; the sink is aborted (after a finish under way)
  Phase phase() const { return _phase; }
  bool settled() const { return _phase == Phase::Settled; }
  UploadResult result() const { return _result; }
  bool failed() const { return _result != UploadResult::Ok; }
  // The producer's data is kept: a session runs, nothing failed, not final.
  bool accepting() const { return _phase == Phase::Running && !failed() && !_final; }
  // Progress (data in, a step done) for the stall check.
  void noteProgress(uint32_t now) { _progressAt = now; }
  bool stalled(uint32_t now, uint32_t limitMs) const { return now - _progressAt >= limitMs; }

  // ---- producer ----
  uint32_t used() const { return _used; }
  uint32_t room() const { return _size - used(); }
  // Room for len bytes in up to two pieces (out[1].len 0 when one does).
  // false when it doesn't fit (fails the session: Overflow), or the data
  // is not kept (!accepting(), no ring).
  bool reserve(uint32_t len, Span out[2]);
  void commit(uint32_t len);  // after reserve(len): the bytes are in
  // After a push, and on a poll: what to do with the TCP window. Hold:
  // don't acknowledge this segment (and keep holding: every segment while
  // held); Reopen: acknowledge all held ones now.
  Window window();
  bool held() const { return _held; }
  uint32_t holds() const { return _holds; }  // windows held this session

  // ---- writer ----
  // The next sink call, or None (nothing to do, or the sink is in use).
  // Marks the sink in use until done(). Write: the step in out[0..1], len
  // bytes in all, ending on a 4 KB boundary of the stream (a write waits
  // until the ring has that much, unless all is in).
  Action next(Span out[2], uint32_t& len);
  bool pending() const;  // next() would give an action
  // The producer writes its chunk itself (the ring is empty, nothing under
  // way): marks the sink in use until done().
  bool claim();
  void opened(bool ok);
  void wrote(uint32_t n, bool ok, bool fromRing);
  void finished(int r);  // UploadSink::kDone / kMore / kFailed
  void aborted();
  void done();
  bool busy() const { return _busy; }
  bool isOpen() const { return _open; }
  bool finishing() const { return _finishing; }  // the first finish call ran, not the last
  uint32_t written() const { return _written; }

 private:
  void settle() { _phase = Phase::Settled; }

  Phase _phase = Phase::Idle;
  UploadResult _result = UploadResult::Ok;
  bool _busy = false;
  bool _copying = false;
  bool _open = false;
  bool _live = false;       // the sink is this session's: neither committed nor aborted
  bool _finishing = false;  // a finish sequence under way
  bool _final = false;
  bool _held = false;
  uint8_t* _buf = nullptr;
  uint32_t _size = 0;
  uint32_t _rd = 0;    // the writer's place in the ring
  uint32_t _used = 0;  // bytes in the ring
  uint32_t _written = 0;  // bytes the sink took this session
  uint32_t _holdBelow = 0;
  uint32_t _holds = 0;
  uint32_t _progressAt = 0;
};

}  // namespace ESPRack

#endif  // ESPRACK_UPLOAD_PUMP_CORE_H
