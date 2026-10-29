#pragma once
#ifndef ESPRACK_UPLOAD_SINK_H
#define ESPRACK_UPLOAD_SINK_H

// UploadSink: where an upload's bytes go (net/UploadPump.h). Every call is
// one bounded flash operation run inside a write gate slot (a FlashWriter
// step) or, while the gate is free, inside a pass on async_tcp: so on
// either task, never two at once, never concurrently with another call of
// the same sink.
//
//   open    before the first write (may be empty). false: failed.
//   write   one step: at most 4 KB (FlashSteps::kSector), once or twice
//           per step (the ring wraps), each step ending on a 4 KB boundary
//           of the stream (system/FlashSteps.h). While the gate is free the
//           upload's own chunks (<= 1460 B) come straight from async_tcp.
//   finish  after the last write; returns kMore to be called again in a
//           slot of its own (one flash operation per call), kDone when
//           committed, kFailed. May be empty (= kDone).
//   abort   the upload failed or its client went: discard what was
//           written (may be empty). One flash operation at most.
//
// A firmware image: OtaWriter::sink() (system/OtaWriter.h). A file:
//   open    f = fs.open(path + ".partial", "w"); f.setBufferSize(0) (steps
//           are whole 4 KB: no 4 KB stdio buffer in internal RAM); return f
//   write   return f.write(data, len) == len
//   finish  1st call: f.close(); return kMore. 2nd: remove the old file if
//           any, return kMore. 3rd: rename .partial -> path; kDone/kFailed
//   abort   f.close(); fs.remove(path + ".partial")
// Arduino-free.

#include <stddef.h>
#include <stdint.h>

#include <functional>

namespace ESPRack {

struct UploadSink {
  static constexpr int kDone = 0;
  static constexpr int kMore = 1;
  static constexpr int kFailed = -1;

  std::function<bool()> open;
  std::function<bool(const uint8_t* data, size_t len)> write;
  std::function<int()> finish;
  std::function<void()> abort;
};

}  // namespace ESPRack

#endif  // ESPRACK_UPLOAD_SINK_H
