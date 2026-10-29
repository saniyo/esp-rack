# Loop and waits

How ESPRack code waits (HGW-LIB-003): the rule, how the loop task waits, the
conversion patterns, and every `delay()` / `vTaskDelay` / `portMAX_DELAY`
that is still in esp-rack and esp-rack-modules, with the task it runs in and
why it stays.

> **Status: 2026-10-29, after the HGW-LIB-003 review fixes.** The list in
> section 4 was taken from the final code of both repos (core, ble, telegram,
> history and esp-rack-modules merged). Line numbers drift: re-run the grep in
> section 4 before relying on one.

---

## 1. The rule

Everything runs under FreeRTOS, and nothing sleeps to wait for something:

- **A wait for a flag** becomes an event: a task notification, a semaphore or
  an event group, with the same timeout the sleep had. The waiter can then be
  woken at once: stop, new settings, the write gate.
- **Socket polling** becomes `select()` with a timeout, or a callback.
- **Long work cut into chunks** yields through an event, not a sleep.
- **Every wait on a lock or an event in a shared context** has a timeout and
  an error path. Shared contexts: `loopTask`, `async_tcp`, `tcpip`,
  `arduino_events`, `sys_evt`.
- **What stays on purpose** (a retry every N s in a module's own
  low-priority task, a hardware settle time) is written as an event wait with
  that timeout and listed in section 4.

Two framework rules apply to every conversion:

- **Task notifications.** Index 0 of a task belongs to its owner (its own
  events). Never notify a task you do not own. To wake another module or the
  loop, use a semaphore, an event group or `ESPRack::wakeLoop()`.
- **Heap objects.** No `std::atomic` read-modify-write in members of heap
  objects (PSRAM on Xtensa). Use loads and stores, or a `portMUX`.

---

## 2. How the loop waits

`App::loop` runs its own work and every module's `onLoop()`. Then it blocks
until the first of:

- the smallest `loopWaitMs()` of all modules;
- the core's own deadlines: TimeSync, BootGuard, a requested restart, and the
  WebSocket manager (`WsManager::waitMs`: 0 while frames are queued, else the
  next heartbeat (5 s) or ping (20 s));
- half the loop watchdog's timeout, while it is armed;
- `ESPRACK_LOOP_MAX_WAIT_MS`;
- an `ESPRack::wakeLoop()`.

The API:

| Name | Where | Meaning |
|---|---|---|
| `ESPRack::wakeLoop()` | `App.h` | Any task: the loop's wait ends now. Cheap, no allocation, safe before `App::begin` and after shutdown. |
| `ESPRack::wakeLoopFromISR()` | `App.h` | The same, from an ISR. |
| `virtual uint32_t Module::loopWaitMs(uint32_t nowMs) const` | `Module.h` | How long this module's `onLoop` may wait from `nowMs` before it must run again (its nearest timer). 0 = run again at once. |
| `ESPRACK_LOOP_DEFAULT_WAIT_MS` (10) | `system/LoopWait.h` | What the default `loopWaitMs` returns for a module that overrides `onLoop()` but not `loopWaitMs()`: unconverted modules, consumers' own included, work as before, only without a busy loop. |
| `ESPRACK_LOOP_IDLE_WAIT_MS` | `system/LoopWait.h` | For an `onLoop` that only reacts to flags set by other tasks; the code that sets the flag calls `wakeLoop()`. Also what the default `loopWaitMs` returns for a module with no `onLoop()` of its own (the base `onLoop()` marks it idle on its first pass): features, security, restart, factory-reset, upload-firmware, system-status, config-manager-ui, ui-dyn, filesystem, history, discovery, web-endpoints, ws-diag. |
| `ESPRACK_LOOP_MAX_WAIT_MS` (1000) | `system/LoopWait.h` | Bounds every wait. |

A module's part:

1. Return the time to its nearest timer from `loopWaitMs()`. Return 0 while a
   flag is pending.
2. Call `ESPRack::wakeLoop()` wherever another task sets a flag that `onLoop`
   reacts to. Do it after a spinlock is released, never inside one.
3. Move slow or blocking work off the loop to the module's own task. Notify
   that task to start the work.

### What the esp-rack-modules return

| Module | `loopWaitMs` |
|---|---|
| rs485-port | The page's 1 s tick only. New settings go to the port task (section 3). |
| can-twai | The driver status every 20 ms while the driver runs, and the page. |
| can-mcp2515 | The chip status every 20 ms while the chip runs, and the page. |
| i2c-bus | The device list every 2 s, and the page. |
| pack-switch | The time to the next 50 ms step, and the page. |
| inverter-link | A failed start is retried after 5 s; the page. |
| jk-bus | A held or failed port is retried at `_retryAtMs`; the page. |
| jk-ble | 250 ms while connected (the requests are seconds apart); the page. |
| board-health | The supply sampler, the slow tick, the 200 ms stats and the page, whichever is due first. |
| dht-sensor | The next read; 5 ms while a read waits for a quiet line; the page. |
| opentherm-boiler | The end of the activation pause; the next request (`OtMaster::quietUntilMs`); 10 ms while an answer is in flight; the page. |
| digital-io | 10 ms (the default) while a device is installed: inputs are sampled and LED / kick pulses are timed on the loop. |
| energy-meter (PZEM) | 10 ms (the default) while running: the Modbus exchange is polled. |
| onewire-temp | 10 ms (the default) while running: the bus slices and the conversion wait. |

Every module above returns 0 while its `_applyPending` (or a similar flag) is
set, and calls `wakeLoop()` where that flag is set.

"The page" means: while the page has subscribers, the time to the next live
push (250 ms / 500 ms when there is news, 1 s / 2 s otherwise). Without
subscribers the page sets no deadline.

The last three keep the default pace: their engines do not report a next
deadline yet. A `nextDueMs()` on `Pzem004tDriver`, `OwtEngine` and the
digital-io devices would let them sleep longer.

---

## 3. Conversion patterns

- **A flag polled in `onLoop`.**
  - Before: `onLoop` checks `_applyPending` (set by an `async_tcp` handler)
    on every pass.
  - After: the handler sets the flag and calls `wakeLoop()`, and
    `loopWaitMs()` returns 0 while the flag is set.
- **Slow work on the loop task** (rs485-port's restart on new pins).
  - Before: `onLoop` ran `end()` + `begin()`, blocking the loop up to about
    1.5 s.
  - After: the settings handler hands the new settings to the port task
    (under its `portMUX`) and notifies it. The task reopens the UART in place
    and keeps the owner's queue.
  - The task handle is cleared under the same `portMUX` before the task is
    stopped, so a late notify never reaches a deleted task.
- **A sleep in a retry loop of a task we do not own** (CAN transports in the
  jk-bus and hv-inverter-can tasks).
  - The sleep becomes `xSemaphoreTake(_wake, same timeout)` on a semaphore
    the transport owns.
  - `begin()` / `end()` give the semaphore, so the waiter wakes early on a
    restart or a stop.
- **Waiting for readers to leave a driver** (can-twai `stopDriver`).
  - Before: `while (busy) vTaskDelay(1)`.
  - After: the last reader out sets an event-group bit, and the stopper waits
    on that bit with a timeout.
  - On a timeout the driver stays installed: inactive, no reader gets in, and
    the next stop tries again. It is never freed under a reader.
- **A read on a stopped port** (can-twai `read()`).
  - Before: the read slept in slices.
  - After: it waits on the `kUp` event-group bit for the slice, so it
    receives as soon as the port starts.
- **A settings copy under the state lock.** `StatefulService::read()` /
  `update()` take the state lock with `ESPRACK_STATE_LOCK_TIMEOUT_MS` (2 s);
  busy, `read()` does not run the reader and returns false. A copy that feeds
  hardware or a save checks it and keeps what runs:
  - a module's `apply()` (can-twai, can-mcp2515, i2c-bus, board-health,
    dht-sensor, digital-io, energy-meter, inverter-link, jk-ble, jk-bus,
    onewire-temp, opentherm-boiler, pack-switch, rtc-ds3231, template-ext-hw)
    sets `_applyPending` again and returns: the next loop pass tries again;
  - `startDriver()` / `startChip()` / `startBus()` / rs485 `begin()` return
    false (rs485 notes "settings busy");
  - rs485 `applySettings()` (on `async_tcp`) leaves the port task's settings
    alone and has the loop run it again; NTP `configureNTP()` sets
    `_reconfigure` again; BLE sets `_applyPending` again; history skips the
    pass (no retention on a default cap);
  - ConfigManager never writes an unread (empty) state: the save stays
    deferred and the FlashWriter tries it again in `CFGMGR_BUSY_RETRY_MS`
    (1 s); the backup step answers `Backup::Busy`; the CRC cache keeps its
    old value;
  - the MQTT message handler (`async_tcp`) drops that seen-topics row update
    when `beginTransaction()` times out.
- **A mutex with `portMAX_DELAY`.**
  - The mutex is taken with a timeout longer than the longest hold.
  - Error path:
    - `begin()` returns false;
    - `end()` logs;
    - `apply()` on the loop sets `_applyPending` again;
    - a poll skips the pass.

---

## 4. What remains

Found with this grep (comments and `delayMicroseconds` left out):

```
grep -rnE "\bdelay\(|vTaskDelay|portMAX_DELAY" --include=*.cpp --include=*.h --include=*.hpp lib modules
```

### 4.1 Exceptions: they stay

| File:line | Task | What | Why it stays |
|---|---|---|---|
| esp-rack `lib/ESPRack/src/power/FlashWriter.cpp:66,73` | flash writer (own task) | `portMAX_DELAY` is the "no timed job" value for `ulTaskNotifyTake`. | The task's own notification. Every job submission notifies it; with no timed job there is nothing to time out. |
| esp-rack `modules/ble/src/BleService.cpp:970, 1038` | BLE task (own) | `ulTaskNotifyTake(portMAX_DELAY)` parks the task after the shutdown (links dropped, `shutdownSem` given). | The chip restarts right after the shutdown hooks; the task has nothing left to do. Its own notification. ble is not installed by the gateway. |
| esp-rack-modules `inverter-link/include/InverterLink/InverterCanTransport.h:95` | hv-inverter-can link task | `vTaskDelay` | Fallback only when the transport's `_wake` semaphore could not be created at all. The normal path waits on `_wake`. |
| esp-rack-modules `jk-bus/src/JkBusModuleService.cpp:113` | jk-dz01 bus task | `vTaskDelay(1)` | Same fallback for `CanPortTransport::_wake`. |
| esp-rack-modules `rs485-port/src/Rs485PortService.cpp` `run()` | rs485 port task (own) | `ulTaskNotifyTake` with 100 ms (open, no deadline) or 1 s (closed) | The task's own notifications: ISR, `send()`, stop, new settings. The timeouts are housekeeping only. Not a sleep. |

### 4.2 Still to convert: not exceptions

| File:line | Task | What | Note |
|---|---|---|---|
| esp-rack `lib/ESPRack/src/ConfigManager.cpp:242` | any (`ConfigManager::lock()`) | mutex `portMAX_DELAY` | Review item 24. Core work; `tryLock(ms)` exists next to it. |
| esp-rack `modules/history/include/HistoryService.h:119`, `src/HistSeriesImpl.cpp:33` | loop, `async_tcp`, producers | RAII `Lock` with `portMAX_DELAY` | Data sections of microseconds. |
| esp-rack `modules/telegram/src/TelegramService.cpp:813` (`publishCommands`), `:869` (`pollCommands`) | Telegram worker (own) | `_cmdMx` `portMAX_DELAY` | Own task. Becomes a timeout if any shared context takes `_cmdMx`. |
| esp-rack `modules/ble/src/BleService.cpp:157` (`RadioLock`), `:192, 962, 978, 1101, 1255` (`writeMx`) | BLE task, loop, `async_tcp` | `mx` / `writeMx` `portMAX_DELAY` | ble is not in the gateway; listed for completeness. (`end()`'s wait, the disconnect wait and the pause are event waits now.) |
| esp-rack-modules `can-mcp2515/include/CanMcp2515Service.h:127` (`Lock`, on `_mx` and `_chip`) | loop (`apply`, `pollChip`), consumer task (read/write, begin/end) | RAII `Lock` with `portMAX_DELAY` | Not in the gateway. Same treatment as can-twai: a timed `Lock` with `ok` and an error path per site. |
| esp-rack-modules `can-mcp2515/src/CanMcp2515Service.cpp:578` (`openBus`), `src/Mcp2515Driver.cpp:75` (`sleepMs`), `:196` (mode-change poll) | loop (`apply`) or the consumer's task (`begin`) | `delay(1)` after RESET, `vTaskDelay` 5 / 20 ms settle | Hardware settle times; the chip has no event for them. Bounded at 25 ms, but on the loop task when `apply()` restarts the chip. Move the restart to the consumer's / a module task, or keep and list it as an exception. |
| esp-rack-modules `inverter-link/include/InverterLinkRackService.h:111`, `jk-ble/include/JkBleService.h:74`, `jk-bus/include/JkBusModuleService.h:147` | loop, `async_tcp` (page), consumer and library tasks | RAII `Lock` with `portMAX_DELAY` | Sections of microseconds (copies, posts to the library's queue), about 40 sites. Converting them needs one framework-wide decision on the error path of such locks (skip with a default value, or log and reboot past a long timeout). The same applies to core `HwRegistry::lock` and `LoopWdt::Lock`. |

---

## 5. Checklist for a new module

- [ ] `onLoop()` has a `loopWaitMs()`: the nearest timer, 0 while a flag is pending.
- [ ] Every flag another task sets for `onLoop` is followed by `ESPRack::wakeLoop()`, outside spinlocks.
- [ ] No `delay()` / `vTaskDelay` on the loop task, in HTTP / WS handlers or in Wi-Fi / SNTP callbacks.
- [ ] Work longer than a few ms runs in the module's own task, which is notified.
- [ ] Every lock or event wait in a shared context has a timeout and an error path.
- [ ] A deliberate wait in the module's own task is an event wait (a stop ends it) and is listed in section 4.1.
