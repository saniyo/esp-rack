#pragma once
#ifndef ESPRACK_HW_IRQ_TIMED_H
#define ESPRACK_HW_IRQ_TIMED_H

// IIrqTimed — interrupt timing between external HW instances.
//
// Some drivers decode their input from interrupt timing: an edge ISR that
// measures pulse widths (OpenTherm, IR remotes, 433 MHz receivers ...).
// Others mask interrupts for milliseconds (a bit-banged AM2302/DHT22 read:
// about 4 ms). On one core the two must not overlap: an edge that comes while
// interrupts are masked is serviced late, and the frame being received is
// corrupt.
//
// A timing-sensitive instance answers queryInterface(IIrqTimed::kIfaceId).
// A driver about to mask interrupts for longer than ~100 us asks all of them,
// on loopTask, right before it masks:
//
//   const char* busy = nullptr;
//   if (!ESPRack::irqQuiet(hw(), &busy)) return;  // try again on the next pass
//   ... mask, read, unmask ...
//
// Both sides run on loopTask (onLoop), so nothing can start between the
// question and the read. An implementation keeps quiet moments coming: the
// OpenTherm master leaves at least 100 ms between frames, so a masking
// driver waits at most about one frame. A masking driver never forces its
// way in; it reports how long it has been waiting instead.

#include "hw/HwRegistry.h"

namespace ESPRack {

class IIrqTimed {
 public:
  static constexpr const char* kIfaceId = "irqTimed.v1";

  virtual ~IIrqTimed() = default;

  // true: nothing is being received by interrupt timing now, and nothing
  // can start before this instance's next onLoop, so masking interrupts now
  // cannot corrupt a frame. loopTask only.
  virtual bool irqQuiet() const = 0;
};

// true when no installed instance is receiving by interrupt timing (loopTask).
// `busy`: the id of the first instance that is, for a status line; nullptr
// when all are quiet.
inline bool irqQuiet(const HwRegistry* hw, const char** busy = nullptr) {
  const char* first = nullptr;
  if (hw) {
    hw->forEach<IIrqTimed>([&first](IIrqTimed& t, const char* id) {
      if (!first && !t.irqQuiet()) first = id;
    });
  }
  if (busy) *busy = first;
  return first == nullptr;
}

}  // namespace ESPRack

#endif  // ESPRACK_HW_IRQ_TIMED_H
