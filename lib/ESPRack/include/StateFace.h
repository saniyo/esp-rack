#pragma once
#ifndef ESPRACK_STATE_FACE_H
#define ESPRACK_STATE_FACE_H

// StateFace — why a state reader runs (SES-LIB-033).
//
//   Rest        GET /state, the POST /state echo, rest.proxy: one reply,
//               sized by sendAdaptiveJson. A static-schema page reads ALL its
//               values from it.
//   WsSnapshot  the frame a client gets when it subscribes to a live channel
//   WsPush      a broadcast to every subscriber (broadcastWs)
//
// The WS faces go out in the channel's fixed frame buffer, so a reader may
// send only what changed there (rows a table upserts) and the full lists in
// the Rest face. Readers that don't ask behave as before.
//
// WsManager sets the WS faces while it builds a frame (one at a time, under
// its frame lock), keyed by the building task — not thread_local, which
// faults on the RISC-V toolchain (see ConfigManager.cpp) — so a Rest read on
// another task at the same moment is never mistaken for a WS one.

#include <stdint.h>

namespace ESPRack {

enum class StateFace : uint8_t { Rest = 0, WsSnapshot, WsPush };

// The face of the state read running on this task.
StateFace stateFace();

// WsManager: the face of the frame this task builds, for the scope's life.
class StateFaceScope {
 public:
  explicit StateFaceScope(StateFace face);
  ~StateFaceScope();
  StateFaceScope(const StateFaceScope&) = delete;
  StateFaceScope& operator=(const StateFaceScope&) = delete;

 private:
  void* _prevTask;
  StateFace _prevFace;
};

}  // namespace ESPRack

#endif  // ESPRACK_STATE_FACE_H
