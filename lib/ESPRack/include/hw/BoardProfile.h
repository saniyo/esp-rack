#pragma once
#ifndef ESPRACK_HW_BOARD_PROFILE_H
#define ESPRACK_HW_BOARD_PROFILE_H

// The running board's pin facts (LIB-FEAT-001): the chip from
// CONFIG_IDF_TARGET_*, the flags from sdkconfig and the runtime
// (PSRAM found, octal PSRAM/flash, USB-Serial-JTAG / CDC console, UART0
// console). The tables themselves are in hw/BoardTables.h and are checked
// against the SoC headers at compile time in BoardProfile.cpp.
//
// The console UART is the IDF console's, CONFIG_ESP_CONSOLE_UART_NUM: ESP_LOGx,
// the core's log_x and the ROM print there. The prebuilt Arduino libs keep it
// on UART0 even with ARDUINO_USB_CDC_ON_BOOT=1, which only moves Serial to
// USB. Without CDC on boot Serial is UART0 too. So on the prebuilt libs UART0
// and its pins are always reserved. A custom sdkconfig with the console on
// USB or off (CONFIG_ESP_CONSOLE_NONE) frees them. A console on UART1/2 is
// reserved as that UART, with its custom pins. Override UART0 with
// -D ESPRACK_HW_UART0_CONSOLE=0/1.

#include "hw/HwClaims.h"

namespace ESPRack {

HwChip currentChip();
BoardFlags currentBoardFlags();

// The IDF console's UART: num -1 = on USB or off. tx/rx: its custom pins
// (CONFIG_ESP_CONSOLE_UART_CUSTOM), else -1 (UART0's are in the tables).
struct ConsoleUart {
  int8_t num = -1;
  int8_t tx = -1;
  int8_t rx = -1;
};
ConsoleUart currentConsoleUart();
BoardInfo currentBoardInfo();  // boardInfoFor(currentChip(), currentBoardFlags())

}  // namespace ESPRack

#endif  // ESPRACK_HW_BOARD_PROFILE_H
