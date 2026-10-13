#pragma once
#ifndef ESPRACK_HW_BOARD_PROFILE_H
#define ESPRACK_HW_BOARD_PROFILE_H

// The running board's pin facts (LIB-FEAT-001): the chip from
// CONFIG_IDF_TARGET_*, the flags from sdkconfig and the runtime
// (PSRAM found, octal PSRAM/flash, USB-Serial-JTAG / CDC console, UART0
// console). The tables themselves are in hw/BoardTables.h and are checked
// against the SoC headers at compile time in BoardProfile.cpp.
//
// UART0 counts as the console unless Serial is USB CDC
// (ARDUINO_USB_CDC_ON_BOOT=1). Override with -D ESPRACK_HW_UART0_CONSOLE=0/1.

#include "hw/HwClaims.h"

namespace ESPRack {

HwChip currentChip();
BoardFlags currentBoardFlags();
BoardInfo currentBoardInfo();  // boardInfoFor(currentChip(), currentBoardFlags())

}  // namespace ESPRack

#endif  // ESPRACK_HW_BOARD_PROFILE_H
