#pragma once
// Host-test hooks of the host WriteGate (WriteGateHost.cpp).

#include <stdint.h>

namespace ESPRack {
void writeGateHostSetNow(uint32_t ms);
void writeGateHostReset();  // no holders, clock at 0
}  // namespace ESPRack
