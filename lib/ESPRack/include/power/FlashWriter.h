#pragma once
#ifndef ESPRACK_FLASH_WRITER_H
#define ESPRACK_FLASH_WRITER_H

// FlashWriter: the framework's one task for the flash writes the write gate
// (power/WriteGate.h) put off. A writer on a shared task (async_tcp, the
// loop) never waits for the gate and never retries: when the gate is not
// free it keeps what is to be written (a dirty flag, a queue of its own)
// and kicks its job here. The task waits for kicks (no polling) and runs
// each job it was kicked for; a job takes a WriteSlot per bounded step (it
// may wait for pauses here) and returns
//   kDone      all written;
//   kOnPower   PowerGuard paused writes: run again when it allows them;
//   n          run again in n ms (e.g. a write that waits for a free gate).
// One task for all of them: internal RAM is the scarce part. It starts with
// the first kick, so a device whose gate never puts a write off never
// spends that RAM (a module that holds write gate windows starts it at
// setup). Same on every chip.

#include <stdint.h>

#include <functional>

#ifndef ESPRACK_FLASH_WRITER_STACK
#define ESPRACK_FLASH_WRITER_STACK 6144  // a config save (JSON, encryption, LittleFS) runs here
#endif
#ifndef ESPRACK_FLASH_WRITER_PRIO
#define ESPRACK_FLASH_WRITER_PRIO 1
#endif

namespace ESPRack {

class FlashWriter {
 public:
  static constexpr uint32_t kDone = 0;
  static constexpr uint32_t kOnPower = 0xFFFFFFFFu;
  static constexpr uint8_t kMaxJobs = 12;  // the framework's modules take 8: room for a consumer's
  using Job = std::function<uint32_t()>;

  // At setup (the loop task). -1 when all kMaxJobs are taken.
  static int8_t add(const char* name, Job job);
  // Any task, not an ISR. Cheap: a flag and a notification.
  static void kick(int8_t id);
  // App::begin: runs the jobs PowerGuard held once it allows writes again.
  static void attach();
  // App::loop: only if the task could not start (internal RAM short), try
  // again, once a second at most, while jobs wait. Nothing otherwise.
  static void retry();
  // Start the task now rather than at the first kick (WriteGate::addHolder:
  // writes will be put off; internal RAM is still to be had at setup).
  static void start();

  // The restart's flush: what a job keeps is written by the task that runs
  // the restart (App, after every onShutdown hook, so no module window is
  // open any more), a WriteSlot per step with boundMs at most for a pause,
  // or it goes with the restart. ms(ctx, boundMs): how long that may take
  // now (0: nothing kept), counted in the restart's grace. At setup (the
  // loop task); false when all kMaxRestartFlush are taken.
  static constexpr uint8_t kMaxRestartFlush = 4;
  using RestartFlush = void (*)(void* ctx, uint32_t boundMs);
  using RestartFlushMs = uint32_t (*)(void* ctx, uint32_t boundMs);
  static bool addRestartFlush(RestartFlush fn, RestartFlushMs ms, void* ctx);
  static void restartFlush(uint32_t boundMs);       // App::runFlushOnce
  static uint32_t restartFlushMs(uint32_t boundMs);  // App::flushAllowanceMs

  static uint8_t jobs();
  static const char* name(int8_t id);
  static bool waiting(int8_t id);  // kicked, on a retry or held by PowerGuard
  static bool started();
};

}  // namespace ESPRack

#endif  // ESPRACK_FLASH_WRITER_H
