#pragma once
#ifndef ESPRACK_PURE_COUNTER_TRACKER_H
#define ESPRACK_PURE_COUNTER_TRACKER_H

// CounterTracker — credits deltas of a monotonic hardware counter (an energy
// register) through resets, wraps and glitches without ever crediting an
// unconfirmed jump. Decision table (first matching row wins; a = anchor,
// v = value, h = held value, maxA = maxSinceAnchor, maxH = maxSinceHeld,
// heldMaxA = the maxA passed with the held sample):
//
//  1 !valid                                         0                 First
//  2 epoch != anchorEpoch                           v <= maxA ? v : 0 ExpectedReset
//  3 no held, 0 <= v-a <= maxA                      v - a             Normal
//  4 no held, -negSlack <= v-a < 0 && evidence      0                 NegSlack
//  5 no held, modulo > 0 && a >= modulo-maxA
//              && v <= maxA && modulo-a+v <= maxA   modulo - a + v    Wrap
//  6 no held, otherwise                             0                 Held (count 1)
//  7 held, 0 <= v-a <= maxA                         v - a             GlitchDropped
//  8 held, 0 <= v-h <= maxH       (h<a && h<=heldMaxA ? h : 0) + v-h  ResetConfirmed (h<a)
//                                                                     / JumpConfirmed (h>a)
//  9 held, otherwise, heldCount+1 < maxHold         0                 Held (count+1)
// 10 held, otherwise, heldCount+1 >= maxHold        0                 ForcedRebase
//
// Rows 1-5, 7, 8, 10 move the anchor to v and clear the held sample; 2 also
// takes the new epoch. "evidence" = the caller saw the meter lose power.

#include <stdint.h>

namespace ESPRack {

struct CounterCfg {
  uint64_t modulo = 0;   // the counter wraps at this value; 0 = never
  int64_t negSlack = 0;  // tolerated step back with power-loss evidence
  uint8_t maxHold = 5;   // consecutive held samples before a forced rebase
};

enum class CounterEvent : uint8_t {
  First, Normal, NegSlack, Wrap, Held, GlitchDropped, ResetConfirmed, JumpConfirmed, ExpectedReset, ForcedRebase
};

struct CounterStep {
  int64_t credit;
  CounterEvent ev;
  uint64_t before, after;  // anchor before / after the sample
};

class CounterTracker {
 public:
  explicit CounterTracker(const CounterCfg& cfg) : _cfg(cfg) {}

  void restore(uint64_t anchor, uint16_t epoch, bool valid);
  // maxSinceAnchor / maxSinceHeld: the largest plausible increase since the
  // anchor / the held sample (e.g. max power x elapsed time, in counts).
  CounterStep feed(uint64_t value, uint16_t epoch, int64_t maxSinceAnchor, int64_t maxSinceHeld,
                   bool powerLossEvidence);

  uint64_t anchor() const { return _anchor; }
  uint16_t epoch() const { return _epoch; }
  bool holding() const { return _held; }
  uint8_t heldCount() const { return _heldCount; }
  bool degraded() const { return _heldCount >= 3; }
  bool valid() const { return _valid; }

 private:
  CounterCfg _cfg;
  uint64_t _anchor = 0;
  uint16_t _epoch = 0;
  bool _valid = false;
  bool _held = false;
  uint8_t _heldCount = 0;
  uint64_t _heldValue = 0;
  int64_t _heldMaxA = 0;

  CounterStep rebase(uint64_t v, int64_t credit, CounterEvent ev);
};

}  // namespace ESPRack

#endif  // ESPRACK_PURE_COUNTER_TRACKER_H
