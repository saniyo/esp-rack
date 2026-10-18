#include "StateFace.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

namespace ESPRack {

namespace {

// The task building a WS frame and that frame's face. Frames are built one at
// a time (WsManager's frame lock), so one slot suffices.
std::atomic<void*> s_task{nullptr};
std::atomic<uint8_t> s_face{static_cast<uint8_t>(StateFace::Rest)};

void* currentTask() { return static_cast<void*>(xTaskGetCurrentTaskHandle()); }

}  // namespace

StateFace stateFace() {
  void* t = s_task.load(std::memory_order_acquire);
  if (!t || t != currentTask()) return StateFace::Rest;
  return static_cast<StateFace>(s_face.load(std::memory_order_relaxed));
}

StateFaceScope::StateFaceScope(StateFace face)
    : _prevTask(s_task.load(std::memory_order_relaxed)),
      _prevFace(static_cast<StateFace>(s_face.load(std::memory_order_relaxed))) {
  s_face.store(static_cast<uint8_t>(face), std::memory_order_relaxed);
  s_task.store(currentTask(), std::memory_order_release);
}

StateFaceScope::~StateFaceScope() {
  s_face.store(static_cast<uint8_t>(_prevFace), std::memory_order_relaxed);
  s_task.store(_prevTask, std::memory_order_release);
}

}  // namespace ESPRack
