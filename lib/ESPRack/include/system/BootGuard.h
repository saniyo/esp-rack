#pragma once
#ifndef ESPRACK_SYSTEM_BOOT_GUARD_H
#define ESPRACK_SYSTEM_BOOT_GUARD_H

// BootGuard: boot-loop detection with an advisory safe mode.
//
// Only CRASH boots count: panic, int-wdt, task-wdt, wdt, an unexplained
// esp_restart ("idf") and unknown. Power cuts, brownouts and intended
// restarts (restartDevice reasons) neither count nor reset the run: a site
// whose mains flickers must not end up in safe mode. A boot that stays up
// stableAfterMs (markStable, automatic from App::loop) ends the run.
//
//   quickReboots >= limit  ->  safeMode (sticky, persisted)
//
// safeMode is advisory: the consumer reads info().safeMode at start (e.g.
// keeps its outputs off). It's cleared by clearSafeMode() (also the admin
// action system.clearSafeMode), a new firmware (different ELF SHA) or a
// factory reset — never by a stable run (the code that crashed may be off).
//
// windowMs: kept from the spec's signature, not used — only a stable run
// ends a crash run (by design); each crash boot lasts < stableAfterMs, so
// `limit` of them in a row fit limit × stableAfterMs anyway.
//
// Persisted in NVS namespace "esprack", key "boot": two writes per boot at
// most (begin, markStable), plus one per clearSafeMode. The boot's writes go
// at once; the later ones go through the flash write gate
// (power/WriteGate.h): a gate pass on the loop / async_tcp, else the
// FlashWriter job "bootguard" writes them (the change is in effect in RAM
// at once; while PowerGuard pauses writes it waits there). Without a job
// (all FlashWriter jobs taken) only gate passes write them, never past
// the gate.
//
// History: the last kBootHistory boots with their reason, class, the time
// of the boot and how long the run before it lasted (NVS key "boots": one
// write at begin, one once the clock is valid). The time is set when this
// boot's clock becomes valid (0 = never); the previous run's length comes
// from RTC memory, so a power cut leaves it unknown (0). App::begin() calls begin() with the defaults;
// call it yourself before app->begin() for other limits (first call wins).

#include <esp_system.h>
#include <stdint.h>

class WebManager;

namespace ESPRack {

enum class BootClass : uint8_t {
  PowerOn,      // power-on
  Brownout,     // brownout
  Panic,        // panic       } crash: counts
  IntWdt,       // int-wdt     }
  TaskWdt,      // task-wdt    }
  Wdt,          // other wdt   }
  Unexplained,  // "idf" (esp_restart outside restartDevice), unknown  } crash
  Intended,     // a restartDevice() reason (user, upload, ota, ...)
  Other,        // ext, deep-sleep, usb, jtag
  Count
};
const char* bootClassName(BootClass c);

struct BootInfo {
  esp_reset_reason_t reason;  // esp_reset_reason()
  const char* lastRestart;    // lastRestartReason()
  BootClass bootClass;        // this boot's class
  uint32_t bootCount;
  uint8_t quickReboots;       // crash boots in a row since the last stable run
  bool safeMode;
  bool stable;                // this boot reached stableAfterMs
  // Boots per class since the last stable run (saturating), e.g. a
  // brownout series.
  uint8_t sinceStable[static_cast<int>(BootClass::Count)];
};

constexpr size_t kBootHistory = 8;

struct BootRecord {
  uint32_t bootCount = 0;   // 0 = empty
  uint32_t bootT = 0;       // UTC of the boot, 0 = unknown
  uint32_t prevUpS = 0;     // the run before this boot lasted, s; 0 = unknown
  BootClass bootClass = BootClass::Other;
  char reason[24] = {0};    // lastRestartReason() of that boot
};

namespace BootGuard {

// The last boots, newest first (this one included). Returns how many.
size_t history(BootRecord* out, size_t max);

void begin(uint8_t limit = 4, uint32_t windowMs = 600000, uint32_t stableAfterMs = 120000);
const BootInfo& info();
// A crash class (panic, a watchdog, an unexplained reset): the ones that
// count towards safe mode. E.g. send a report after a crash boot:
//   if (BootGuard::isCrash(BootGuard::info().bootClass)) ...
bool isCrash(BootClass c);
void markStable();
// true: cleared and saved; false: cleared, the save put off by the write
// gate or failed (logged; the admin action answers 500), or not begun.
// Never waits.
bool clearSafeMode();

// Framework plumbing (App).
void loop();                            // markStable() once uptime >= stableAfterMs
// App::loop's wait: ms until loop() is due (markStable, the 10 s uptime
// record). The boot's time waits for a valid clock: TimeSync wakes the loop.
uint32_t loopWaitMs(uint32_t nowMs);
void registerManifest(WebManager* web);  // admin action system.clearSafeMode
// A write the gate put off is still in RAM.
bool savePending();
// Shutdown: writes what is pending now, 2 x boundMs at most for both blobs
// (gate slots, and a write under way on another task, whose end it waits
// for); the FlashWriter job takes no new blob meanwhile. Not on async_tcp
// or the loop task outside shutdown.
void flush(uint32_t boundMs);

}  // namespace BootGuard
}  // namespace ESPRack

#endif  // ESPRACK_SYSTEM_BOOT_GUARD_H
