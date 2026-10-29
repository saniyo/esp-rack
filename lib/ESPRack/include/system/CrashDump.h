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
//
// The erase goes through the flash write gate (power/WriteGate.h): one 4 KB
// sector per gate slot (a 64 KB partition: 16 erases of ~45 ms, up to
// ~0.4 s each, fit no pause between a module's windows and would stall
// async_tcp past its watchdog), in the FlashWriter job "coredump". The
// first sector goes first: the dump is invalid from then on. eraseStatus()
// tells how far it got. The download and the erase exclude each other: a
// download is refused (409) while an erase runs, an erase while a download
// reads the partition. While PowerGuard pauses writes a new erase is
// refused (503); one under way waits and goes on once writes are allowed.
//
// CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE (off by default): IDF marks the
// partition free again only in its own one-call erase, which this one by
// sectors can't be. With the option on, a crash after erase() is saved only
// from the next boot on (a build warning says so).

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

enum class EraseState : uint8_t { Idle, Erasing, Done, Failed };
struct EraseStatus {
  EraseState state = EraseState::Idle;
  uint16_t done = 0;   // sectors erased
  uint16_t total = 0;  // sectors in the partition
};
// Starts the erase (any task, never waits for the gate): true when it is
// under way (or already was) or done, false when there is no coredump
// partition or no FlashWriter job, a download is under way, PowerGuard
// pauses writes, or the first sector failed.
bool erase();
EraseStatus eraseStatus();
const char* eraseStateName(EraseState s);  // "idle", "erasing", "done", "failed"

// Framework plumbing (App): admin actions system.coredump (GET: the file)
// and system.eraseCoredump.
void registerManifest(WebManager* web);

}  // namespace CrashDump
}  // namespace ESPRack

#endif  // ESPRACK_SYSTEM_CRASH_DUMP_H
