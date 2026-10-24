#pragma once
#ifndef ESPRACK_SYSTEM_CRASH_DUMP_H
#define ESPRACK_SYSTEM_CRASH_DUMP_H

// CrashDump: the core dump ESP-IDF writes to the "coredump" partition on a
// panic or a watchdog reset (sdkconfig: to flash, ELF), the same on every
// chip. A summary for the System page and a boot report, the raw image to
// download, and erase.
//
//   char line[160];
//   if (ESPRack::CrashDump::describe(line, sizeof line)) report(line);
//
// The download is the raw partition image ("coredump.bin"). Decode it on a
// PC with the firmware's ELF:
//   esp-coredump info_corefile -t raw -c coredump.bin firmware.elf
//
// The summary is read from flash once and cached; erase() clears it. A new
// crash replaces the dump (one at a time).

#include <stddef.h>
#include <stdint.h>

class WebManager;

namespace ESPRack {
namespace CrashDump {

struct Info {
  bool present = false;     // a valid dump is on flash
  uint32_t size = 0;        // bytes
  char task[16] = {0};      // the task that crashed
  uint32_t pc = 0;          // its program counter
  uint32_t bt[8] = {0};     // backtrace (Xtensa) or return address (RISC-V: bt[0])
  uint8_t depth = 0;
  char reason[64] = {0};    // the panic reason, when the dump has one
  char elfSha[16] = {0};    // the crashed firmware's ELF SHA (prefix, hex)
  bool thisFirmware = false;  // that is the firmware running now
};

// The cached summary (reads the flash on the first call).
const Info& info();
// One line: "loopTask: <reason>, PC 0x…, bt 0x… 0x… (this firmware)".
// false (and "") when there is no dump.
bool describe(char* out, size_t len);
bool erase();

// Framework plumbing (App): admin actions system.coredump (GET: the file)
// and system.eraseCoredump.
void registerManifest(WebManager* web);

}  // namespace CrashDump
}  // namespace ESPRack

#endif  // ESPRACK_SYSTEM_CRASH_DUMP_H
