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
- **Long work cut into chunks** in a module's own low-priority task gives the
  CPU away with a one-tick wait on its own notification (`yieldStep()`), so
  IDLE and lower priorities run (`taskYIELD()` only reaches equal priority).
  That is a one-tick sleep; each such task is listed in section 4.1.
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
  next heartbeat (5 s) or ping (20 s)). Its rx/tx queues (16 each) are pumped
  by the loop task only. A full queue never drops a frame silently: drops are
  counted (`rx_queue_dropped` / `tx_queue_dropped` in the WS stats) and logged
  1st, 2nd, 4th...; a lost broadcast is sent again as the channel's current
  state; a lost incoming frame (a sub, a write) closes that client with 1013,
  so the page reconnects and subscribes again; a snapshot-on-subscribe is
  sent by the loop at once, not queued;
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
    when `beginTransaction()` times out;
  - a feature's GET `/form` and `/state`, its `?schema=1` and the POST echo
    (and the same over rest.proxy) answer 503 `state_busy` with Retry-After,
    never 200 with `{}`: the response is built under one hold of the state
    lock (`StatefulService::withLock`), so it is whole or not sent;
  - a WS write (`{type:"p"}`) the updater refuses, or that finds the state
    busy, gets `{type:"err",for,detail}` and that channel's snapshot, so the
    page shows the device's value again.
- **A mutex with `portMAX_DELAY`.**
  - The mutex is taken with a timeout longer than the longest hold.
  - Error path:
    - `begin()` returns false;
    - `end()` logs;
    - `apply()` on the loop sets `_applyPending` again;
    - a poll skips the pass.

---

## 4. What remains

Found with this grep, in both repos (comment lines left out):

```
grep -rnE "\bdelay\(|vTaskDelay|portMAX_DELAY|delayMicroseconds|esp_rom_delay_us|ets_delay_us|pauseMs\(|yieldStep\(|ulTaskNotifyTake\(|WiFi\.disconnect\(" --include=*.cpp --include=*.h --include=*.hpp lib modules
```

A grep does not see a wait inside a library call. Those found by reading the
Arduino core (3.x) are in section 4.3.

### 4.1 Exceptions: they stay

| File:line | Task | What | Why it stays |
|---|---|---|---|
| esp-rack `lib/ESPRack/src/power/FlashWriter.cpp:66,73` | flash writer (own task) | `portMAX_DELAY` is the "no timed job" value for `ulTaskNotifyTake`. | The task's own notification. Every job submission notifies it; with no timed job there is nothing to time out. |
| esp-rack `modules/ble/src/BleService.cpp:970, 1038` | BLE task (own) | `ulTaskNotifyTake(portMAX_DELAY)` parks the task after the shutdown (links dropped, `shutdownSem` given). | The chip restarts right after the shutdown hooks; the task has nothing left to do. Its own notification. ble is not installed by the gateway. |
| esp-rack-modules `inverter-link/include/InverterLink/InverterCanTransport.h:95` | hv-inverter-can link task | `vTaskDelay` | Fallback only when the transport's `_wake` semaphore could not be created at all. The normal path waits on `_wake`. |
| esp-rack-modules `jk-bus/src/JkBusModuleService.cpp:113` | jk-dz01 bus task | `vTaskDelay(1)` | Same fallback for `CanPortTransport::_wake`. |
| esp-rack-modules `rs485-port/src/Rs485PortService.cpp` `run()` | rs485 port task (own) | `ulTaskNotifyTake` with 100 ms (open, no deadline) or 1 s (closed) | The task's own notifications: ISR, `send()`, stop, new settings. The timeouts are housekeeping only. Not a sleep. |
| esp-rack `lib/ESPRack/src/App.cpp:441` | loop task (`App::begin`, setup) | `pauseMs(50)` between two LittleFS mount attempts | A wait on `kNever`, a bit nobody sets: a 50 ms sleep no event ends. Boot only, before any module runs; a retry has nothing to wait for. |
| esp-rack `lib/ESPRack/src/App.cpp:935` | the second caller of `restartDevice()` (any task, `async_tcp` too; it left the task watchdog first) | `pauseMs(hooks + ESPRACK_RESTART_DEADLINE_MS + 1 s)` on `kNever`, then `esp_restart()` | A backstop: the restart deadline timer resets the chip before it ends. It runs out only when that timer is missing. The task has nothing else to do: a restart is under way. |
| esp-rack `modules/wifi/src/WiFiSettingsService.cpp:599` (`reconfigureWiFiConnection`) | loop task (`begin()`, and a Wi-Fi settings save) | `WiFi.disconnect(false, true)`: Arduino's `STAClass::disconnect` polls `delay(5)` while connected, 100 ms at most | The erase (`esp_wifi_set_config`) fails while the link is still up, so the link must be down first. Settings save only, never in steady state. To convert: `disconnectAsync(false, false)`, then the erase and the reconnect from `ARDUINO_EVENT_WIFI_STA_DISCONNECTED` (with care for a connect `manageSTA` already started). |
| esp-rack `modules/filesystem/src/FileSystemService.cpp:943` (`yieldStep`) | archive and hash tasks (own, priority 1) | `ulTaskNotifyTake(pdTRUE, 1)`, nobody gives it | A one-tick sleep between steps of a long walk, so IDLE (the task watchdog) and lower priorities run. |
| esp-rack `modules/filesystem/include/FileSystemSd.h:187` (`yieldStep`), `:197` | SD job task, SD mount watcher (own, one-shot) | the same one-tick step; 150·i ms (150..750) between 5 mount attempts | Card power-up has no event. The watcher ends after the 5th try. |
| esp-rack own-task cadences: `auto-update/src/AutoUpdateService.cpp:664` (1 s for "success" to reach the page, then the restart), `cert-manager/src/CertManagerService.cpp:537` (enrol retry; Wi-Fi up or a save ends it), `:1409, :1416` (recovery poll; a new trigger ends it), `discovery/src/DiscoveryService.cpp:319, 323, 342` (link re-check; GOT_IP ends it), `history/src/HistoryService.cpp:620` (1 s tick), `mothership/src/MothershipService.cpp:419, 441` (check-in cadence, 5 s at most; `setCadence` ends it), `:1382` (500 ms before a restore restart), `telegram/src/TelegramService.cpp:671, 708, 730` (offline re-check, retry pause, the send interval; a save ends it), `ble/src/BleService.cpp:987, 1208, 1324` (disconnect 2 s at most, 200 ms peer settle, 200 ms idle) | each module's own task | a timed wait on the task's own notification (or a semaphore it owns) | Each is the cadence of that task's work, and the events that matter end it early. None runs on a shared context. |
| esp-rack-modules `pack-switch/src/PackSwitchRackService.cpp:645` | pack-switch step task (own) | `ulTaskNotifyTake(pdTRUE, kStepMs)` | The 50 ms step; a command ends it early. |
| esp-rack-modules `i2c-bus/src/I2cBusService.cpp:474-497` (`clearLines`) | loop task (`apply`, bus recovery) | `delayMicroseconds(5)` per half clock, 9 clocks and a STOP at most: about 100 µs | Bit timing of the recovery clocks; no event times a 5 µs edge. Only when a device holds SDA low. Not in the gateway. |
| esp-rack-modules `onewire-temp/src/EspOneWireBus.cpp:63-100` | loop task (the engine's bus slices) | `esp_rom_delay_us`: reset 480 + 70 + 410 µs, a bit slot about 70 µs, under the bus `portMUX` | 1-Wire slot timing. The engine cuts a conversion into slices, so the loop is held about 1 ms per slice. Not in the gateway. |
| esp-rack-modules `dht-sensor/src/EspDhtLine.cpp:63, 76` | loop task (a read) | `esp_rom_delay_us`: the 1.1 ms start pulse | AM2302 start timing; the answer is then read by edge timestamps. Not in the gateway. |
| esp-rack-modules `can-mcp2515/src/CanMcp2515Service.cpp:576, 578` (`openBus`), `src/Mcp2515Driver.cpp:75, 196` | see 4.2 | `delayMicroseconds(10)` RESET pulse, `delay(1)` oscillator start, 5 / 20 ms mode settle | Listed in 4.2: the restart should leave the loop task. |

### 4.2 Still to convert: not exceptions

| File:line | Task | What | Note |
|---|---|---|---|
| esp-rack `lib/ESPRack/src/ConfigManager.cpp` (`ConfigManager::lock()`) | any | mutex `portMAX_DELAY` | Review item 24. Core work; `tryLock(ms)` exists next to it. |
| esp-rack `modules/history/include/HistoryService.h:119`, `src/HistSeriesImpl.cpp:33` | loop, `async_tcp`, producers | RAII `Lock` with `portMAX_DELAY` | Data sections of microseconds. |
| esp-rack `modules/telegram/src/TelegramService.cpp:813` (`publishCommands`), `:869` (`pollCommands`) | Telegram worker (own) | `_cmdMx` `portMAX_DELAY` | Own task. Becomes a timeout if any shared context takes `_cmdMx`. |
| esp-rack `modules/ble/src/BleService.cpp:157` (`RadioLock`), `:192, 962, 978, 1101, 1255` (`writeMx`) | BLE task, loop, `async_tcp` | `mx` / `writeMx` `portMAX_DELAY` | ble is not in the gateway; listed for completeness. (`end()`'s wait, the disconnect wait and the pause are event waits now.) |
| esp-rack-modules `can-mcp2515/include/CanMcp2515Service.h:127` (`Lock`, on `_mx` and `_chip`) | loop (`apply`, `pollChip`), consumer task (read/write, begin/end) | RAII `Lock` with `portMAX_DELAY` | Not in the gateway. Same treatment as can-twai: a timed `Lock` with `ok` and an error path per site. |
| esp-rack-modules `can-mcp2515/src/CanMcp2515Service.cpp:578` (`openBus`), `src/Mcp2515Driver.cpp:75` (`sleepMs`), `:196` (mode-change poll) | loop (`apply`) or the consumer's task (`begin`) | `delay(1)` after RESET, `vTaskDelay` 5 / 20 ms settle | Hardware settle times; the chip has no event for them. Bounded at 25 ms, but on the loop task when `apply()` restarts the chip. Move the restart to the consumer's / a module task, or keep and list it as an exception. |
| esp-rack-modules `inverter-link/include/InverterLinkRackService.h:111`, `jk-ble/include/JkBleService.h:74`, `jk-bus/include/JkBusModuleService.h:147` | loop, `async_tcp` (page), consumer and library tasks | RAII `Lock` with `portMAX_DELAY` | Sections of microseconds (copies, posts to the library's queue), about 40 sites. Converting them needs one framework-wide decision on the error path of such locks (skip with a default value, or log and reboot past a long timeout). Core `HwRegistry::lock` already waits `ESPRACK_HW_LOCK_WAIT_MS` (500 ms) with an error path per caller; `LoopWdt` has no lock wait left (a spinlock on the pause count, never skipped). |

### 4.3 Waits inside Arduino calls

| Call | Where it is called | The wait | Note |
|---|---|---|---|
| `WiFi.disconnect(wifioff, eraseap)` (default timeout 100) | wifi module, loop task (4.1) | `delay(5)` while connected, 100 ms at most | The event-driven form is `WiFi.disconnectAsync()` (timeout 0); the wifi event handlers use it. |
| `WiFi.begin()`, `WiFi.softAP()`, `WiFi.softAPConfig()` (through `STAClass::begin` / `APClass::begin`) | wifi `manageSTA`, ap `startAP`: loop task | an event-group wait for `ESP_NETIF_STARTED_BIT`, 1 s at most | Only while the interface is not started yet: `App::begin` starts both (`WiFi.mode(WIFI_AP_STA)`). A timed event wait, no polling. |
| `WiFiClient` / `NetworkClient` reads with a timeout | modules' own tasks (MQTT TLS, Telegram, mothership) | `delay(2)` between empty reads | Never on a shared context. `PersistentTlsClient` waits with `select()` instead. |

---

## 5. Checklist for a new module

- [ ] `onLoop()` has a `loopWaitMs()`: the nearest timer, 0 while a flag is pending.
- [ ] Every flag another task sets for `onLoop` is followed by `ESPRack::wakeLoop()`, outside spinlocks.
- [ ] No `delay()` / `vTaskDelay` on the loop task, in HTTP / WS handlers or in Wi-Fi / SNTP callbacks.
- [ ] Work longer than a few ms runs in the module's own task, which is notified.
- [ ] Every lock or event wait in a shared context has a timeout and an error path.
- [ ] A deliberate wait in the module's own task is an event wait (a stop ends it) and is listed in section 4.1.
- [ ] An Arduino call that waits inside (section 4.3) on a shared context has a row in section 4.1.
