#pragma once
#ifndef ESPRACK_UPLOAD_PUMP_H
#define ESPRACK_UPLOAD_PUMP_H

// UploadPump: an upload received on async_tcp (ESPAsyncWebServer's upload
// callback) written into flash through the write gate (power/WriteGate.h)
// without ever waiting on async_tcp. One pump per upload endpoint, one
// upload at a time; a member of the module, attach()ed at setup.
//
//   setup:    pump.attach("fw-upload");                    (the loop task)
//   upload callback (async_tcp):
//     if (!index && !pump.begin(request, sink)) { answer 503 yourself }
//     pump.push(request, data, len, final);
//   request handler (async_tcp):
//     if (!pump.finish(request, respond, closed)) { no upload: answer yourself }
//   respond(request, result) builds the response (nullptr: 200 / 500);
//   closed(result) runs once the client has gone (e.g. restart after a
//   firmware). The sink: net/UploadSink.h (OtaWriter::sink() for a
//   firmware, a .partial file renamed at the end for a file).
//
// How the bytes go:
//   - Gate free (no holder ever opened a window: the default): each chunk
//     is written straight from the upload callback inside a WritePass, as
//     uploads always were; open/finish/abort too. No ring, no task, no
//     change to the request's callbacks.
//   - Otherwise the chunk goes into a ring and a FlashWriter job
//     (power/FlashWriter.h) drains it: one WriteSlot per step of <= 4 KB
//     ending on a 4 KB boundary of the stream (system/FlashSteps.h: one
//     sector erase at most; a step waits until the ring holds a whole
//     one, unless all is in). Open, each finish call and the abort take a
//     slot of their own. Once the gate is free again, due steps run from
//     async_tcp in a WritePass (a few per chunk), the rest on the job.
//     PowerGuard holding writes: the job waits (FlashWriter::kOnPower)
//     and the data stays in the ring.
//   - No FlashWriter job (attach() found the table full): uploads still
//     run while the gate is free; a chunk that would have to wait fails
//     the upload (NoWriter), and a call that waits (finish, the abort)
//     runs from the pump's poll, or the next begin(), once the gate is
//     free.
//   - The ring: ESPRACK_UPLOAD_RING_PSRAM bytes from PSRAM when there is
//     some, else kRingMin from internal RAM (one 4 KB step + TCP_WND + 2
//     MSS: 12.7 KB with the default lwIP settings). Taken at the first
//     chunk that can't go straight through, freed when the upload is over.
//
// TCP back-pressure (AsyncTCP 3.4.5, ESPAsyncWebServer 3.7.10, checked):
//   - The upload callback runs inside the request's onData (AsyncClient::
//     _recv -> request->_onData -> multipart parser -> handleUpload), once
//     per received segment that carries file data (the parser flushes its
//     1460 B buffer on the segment's last byte). When less than
//     holdBelow = TCP_WND + 2 MSS is free after a push, push() calls
//     client->ackLater() for that segment and every later one while held:
//     the peer can send at most TCP_WND more, which fits (one MSS spare
//     for a segment the parser didn't flush).
//   - ack() is not thread-safe (_rx_ack_len races _recv; a freed client
//     after a disconnect), so the window opens again only on async_tcp:
//     in push() (a later segment) or in the pump's poll callback (every
//     ~0.5 s: a zero window brings no more segments), when holdBelow is
//     free again: client->ack(all held).
//   - The poll callback: the first hold (or a deferred answer) replaces
//     the client's onPoll, which the request set in its constructor to
//     call request->_onPoll() (private). That one does nothing until the
//     request has a response (WebRequest.cpp _onPoll: _response != NULL);
//     once the pump has sent the answer its poll does what _onPoll did
//     (more of the response when it can send, close() once it is
//     finished). So: once begin() returned true, answer only through
//     finish(). onData, onAck, onTimeout and onError stay the request's.
//   - Liveness: begin() takes request->onDisconnect() (the request calls
//     it before it deletes itself and its client); the pump forgets the
//     request and client there and never touches them after, and its poll
//     only acts for the client it holds. Don't set request->onDisconnect
//     yourself: pass closed to finish().
//   - Rx timeout: the server closes a connection after 3 s without data
//     (setRxTimeout(3); a held window brings none, and the gate may hold
//     a write for up to ~12 s before it forces it). The pump sets it to 0
//     when it first holds the window and watches the upload itself: no
//     progress (data in, a step written) for ESPRACK_UPLOAD_STALL_MS fails
//     it (Stalled) and gives the timeout back. When the request ends
//     before the flash work does, finish() pauses it (request->pause():
//     no rx timeout, no automatic 501) and the pump answers from its poll
//     once the result is known (a failure: at once, the abort may follow).
//
// A failure (the sink, overflow, no memory, stall) drops the rest of the
// body (the window opens again), aborts the sink (opened or not) and is
// answered with finish(). A client that goes aborts the sink (in a slot,
// or straight away while the gate is free); closed() gets Aborted. A
// failure once the sink's finish has begun (a client that goes, a stall
// between two finish calls): the finish runs to its end (a file is put in
// place, an image set to boot), the result stays the failure.
// ESP32 family only (AsyncTCP, IDF heap caps).

#include <ESPAsyncWebServer.h>
#include <freertos/FreeRTOS.h>
#include <stddef.h>
#include <stdint.h>

#include <functional>

#include "net/UploadPumpCore.h"
#include "net/UploadSink.h"

#ifndef ESPRACK_UPLOAD_RING_PSRAM
#define ESPRACK_UPLOAD_RING_PSRAM 32768  // from PSRAM when there is some (more steps between holds)
#endif
#ifndef ESPRACK_UPLOAD_STALL_MS
#define ESPRACK_UPLOAD_STALL_MS 30000  // no progress this long: the upload fails (Stalled)
#endif

namespace ESPRack {

class UploadPump {
 public:
  using Respond = std::function<AsyncWebServerResponse*(AsyncWebServerRequest* request, UploadResult result)>;
  using Closed = std::function<void(UploadResult result)>;

  static const uint32_t kHoldBelow;  // TCP_WND + 2 MSS
  static const uint32_t kRingMin;    // one 4 KB step + kHoldBelow

  UploadPump() = default;
  UploadPump(const UploadPump&) = delete;
  UploadPump& operator=(const UploadPump&) = delete;

  // At setup (the loop task): the pump's FlashWriter job. name: static.
  // false: no job left (uploads then run only while the gate is free).
  bool attach(const char* name);

  // ---- async_tcp only ----
  // The upload callback's first chunk (index 0), after the caller's own
  // checks (auth, size). false: another upload runs (or its client is
  // still there, or its sink is still being aborted): the caller answers
  // (e.g. 503).
  bool begin(AsyncWebServerRequest* request, UploadSink sink);
  // Every chunk, the last one with final (the upload callback's are
  // <= 1460 B; a chunk over 4 KB never goes straight through). false: not
  // this pump's request, or the upload failed (the data is dropped;
  // finish() answers).
  bool push(AsyncWebServerRequest* request, const uint8_t* data, size_t len, bool final);
  // The request handler. false: no upload of this request (the caller
  // answers). Else the pump answers with respond(request, result) now or,
  // once the flash work is done, from its poll, and calls closed(result)
  // when the client has gone.
  bool finish(AsyncWebServerRequest* request, Respond respond, Closed closed = nullptr);
  bool owns(const AsyncWebServerRequest* request) const { return request && request == _req; }

  // ---- any task ----
  bool active() const;          // an upload runs (or its sink is still being aborted)
  UploadResult result() const;  // the current or last upload's
  uint32_t written() const;     // bytes the sink took
  uint32_t holds() const;       // TCP windows held (this upload)

 private:
  struct Lock {
    explicit Lock(portMUX_TYPE& m) : _m(m) { portENTER_CRITICAL(&_m); }
    ~Lock() { portEXIT_CRITICAL(&_m); }
    portMUX_TYPE& _m;
  };

  static void pollThunk(void* arg, AsyncClient* c);
  void poll(AsyncClient* c);
  void gone(uint32_t gen);
  uint32_t run();
  void act(UploadPumpCore::Action a, UploadPumpCore::Span s[2], uint32_t len);
  void driveFree();
  bool ensureRing();
  void reclaim();
  void watch();
  bool respondIfReady();

  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
  UploadPumpCore _core;  // under _mux
  UploadSink _sink;      // set by begin() while no session runs; called by whoever holds the sink
  int8_t _job = -1;

  // async_tcp only
  AsyncWebServerRequest* _req = nullptr;
  AsyncClient* _client = nullptr;
  AsyncWebServerResponse* _resp = nullptr;  // sent by the pump; the request owns it
  Respond _respond;
  Closed _closed;
  uint32_t _gen = 0;
  uint32_t _rxTimeout = 0;  // the client's before the pump turned it off
  bool _finishCalled = false;
  bool _watching = false;
};

}  // namespace ESPRack

#endif  // ESPRACK_UPLOAD_PUMP_H
