# Heap baseline regression on C3 — root cause + fix plan

**Status:** plan only. No code edits until approved.

## Observed symptom

ESP32-C3 (320 KB SRAM, no PSRAM). Live measurements from
`HeapMonitor::logSnapshot` (after the bug where it read stale ring
data was fixed):

| Moment | `free` | `max_alloc` | `min_free_ever` |
|---|---|---|---|
| Boot peak (1st ring tick, ~1 min) | ~210 KB | **114 KB** | 200 KB |
| Post first TLS handshake (~7 s after boot) | 42 KB | **9 KB** | 15 KB |
| ~3 min uptime with browser open | 10 KB | **4 KB** | **0.86 KB** |

In 7 s after boot we lose **105 KB of contiguous heap**. Web SPA
returns 503 because `WebManager::ensureManifestCache` refuses to
reserve when `max_alloc` is already below its threshold. Mothership
TLS reconnect fails (`TCP connect failed` — no room for socket
state). Device is 1-2 requests away from OOM panic at all times.

## Root cause — the 105 KB cliff, decomposed

(Source: subagent code audit + observed log trajectory.)

1. **WiFi/lwIP STA bring-up** — `WiFi.mode(WIFI_AP_STA)` in
   [App.cpp:208](../../lib/ESPRack/src/App.cpp). The esp_wifi driver
   allocates several 4-16 KB AMPDU RX/TX queues + lwIP netif state.
   Roughly **~30 KB**, several medium-large blocks. Permanent.
2. **FreeRTOS task stacks for long-running services**:
   - Mothership task — **8 KB single contiguous block**,
     `xTaskCreatePinnedToCore` in `MothershipService.cpp:300`. Allocated
     from heap; sits in the middle of the heap as a permanent hole.
   - Telegram task — **6 KB single contiguous block** in
     `TelegramService.cpp:202-204`.
   - Combined: **~14 KB of permanent holes** at predictable mid-heap
     positions. These are the kind of allocations that hurt the most:
     not the size itself, but the holes they punch out.
3. **AsyncWebServer per-handler chain** — every `srv->on(path, …)`
   allocates a handler node (~80-150 B). PROGMEM_WWW registers ~40-80
   routes (one per asset), plus every module adds ~5 more. ~6-12 KB
   scattered (fragmenter, not a single hole).
4. **18-module `onBegin` loop's `DynamicJsonDocument`s**
   — each module's `_cfg.ensureLoaded` deserializes a 1-4 KB JSON,
   then the doc is destroyed. Each leaves a hole the size of the doc
   it just freed. Cumulative residue ~20 KB scattered.
5. **First TLS handshake transient** — BearSSL static buffers are in
   BSS (no heap), but mbedtls-style ASN.1/ECDSA verify allocates ~10-15
   blocks (256 B-8 KB) during cert chain validation, then frees them.
   Holes survive. Matches the 114→9 KB cliff at exactly t=7 s.
   ~25 KB residue.
6. **WebManager manifest cache** (my recent change) — 5-8 KB single
   contiguous block built by incremental `std::string::reserve()`
   inside `WebManager.cpp:112-242`. The "incremental" part is also a
   realloc churner.
7. **Misc**: LittleFS cache (~4-8 KB), TLS PEM Strings (~5 KB),
   `WebFeatureEntry` registrations with String-heavy
   `WebFeatureSpec` captures (~3-5 KB).

Total: ~80-100 KB explained — matches the observed 105 KB cliff
within margin.

## What's NOT the cause (rule-outs)

- mDNS responder (~5 KB, lazy after STA gets IP — not the boot cliff)
- WG library (measured at +1.6 KB only when tunnel comes up; previous
  "WG=160 KB" hypothesis was disproved by live HeapMonitor logs)
- PROGMEM React bundle content (in flash, not heap — only the per-file
  handler nodes in AsyncWebServer's chain count)
- ESP_SSLClient buffers (in BSS via `STATIC_IN/OUT_BUFFER_SIZE`)

## Phase 0 — instrumentation (do FIRST, no fixes)

Goal: PROVE which phase eats what. The numbers above are
synthesized from code audit + log fragments — they're hypotheses,
not yet measurements. Don't fix anything blindly.

Add `HeapMonitor::logSnapshot("boot:<tag>")` calls in
`App::begin()` after each numbered phase:
- `boot:fs-mounted` (after `ESPFS.begin(true)`)
- `boot:wifi-mode-set` (after `WiFi.mode(WIFI_AP_STA)`)
- `boot:modules-installed` (after the install loop)
- `boot:modules-begun` (after each module's `onBegin` — inside loop)
- `boot:webmanager-begun`
- `boot:server-started` (after `server.begin`)

Plus per-module-`onBegin` granularity:
```cpp
for (auto& m : modules_) {
  m->onBegin();
  log_i("[boot] post-%s: free=%u max_alloc=%u",
        m->describe().id, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}
```

Run device, capture log, attach as Appendix. Without these numbers
we're patching blind.

## Phase 1 — kill the two single biggest holes

Task stacks for long-running services use heap-allocated stacks by
default. Switch Mothership + Telegram to `xTaskCreateStatic` with a
**BSS-resident static buffer**. Cost: same 14 KB of memory but
relocated from heap → BSS, so `max_alloc` is no longer affected.

Files:
- `modules/mothership/src/MothershipService.cpp:300-307`
- `modules/telegram/src/TelegramService.cpp:202-204`

Expected gain: +14 KB worth of contiguous-heap holes removed.
Risk: tasks already running; needs careful one-shot init.

## Phase 2 — single shared DynamicJsonDocument for ConfigManager

The 18-module `_cfg.ensureLoaded` loop currently has each module own a
transient `DynamicJsonDocument(4096)`. Each alloc-and-free leaves a
hole. Fix: ConfigManager holds ONE module-shared doc that gets
`clear()`'d between modules. One alloc, reused, freed at end of
`App::begin()`.

Files: `lib/ESPRack/include/ConfigManager.h` (the `Handle::load` path).

Expected gain: ~15-20 KB of fragmentation holes eliminated.
Risk: medium — touches every settings module.

## Phase 3 — fix the manifest cache approach

Two changes:
1. Stop using `std::string::reserve()` incrementally — that's a
   fragmenter. Allocate ONCE with the final size known. Either:
   (a) measure the JSON twice (`measureJson(doc)` on a fully-built
   `JsonDocument`, then `serializeJson` into a pre-sized buffer), or
   (b) use a `std::vector<char>` with a known up-front capacity.
2. Build manifest in `WebManager::begin()` — already done in the
   current branch, just need to verify it actually fires before the
   first browser request.

Files: `lib/ESPRack/src/WebManager.cpp:112-242`.

Expected gain: eliminate the realloc-churn pattern, ~5 KB of holes
that the current code creates.

## Phase 4 — audit `WebFeatureSpec` Strings

The `WebFeatureSpec` struct captures String fields per entry (id,
title, icon, route). 15 entries × ~4 String fields × ~20 B each = ~1
KB of small heap fragments. Migrating these to `const char*` pointing
at module-owned static literals is one of the cheapest wins.

Files: `lib/ESPRack/include/WebFeatureDelegate.h` and consumers.

## Phase 5 — handler chain consolidation (NICE TO HAVE, not blocker)

PROGMEM_WWW registers one `srv->on(path, …)` per asset (40-80 routes
in a typical React build). Each is a heap-resident node. Possible:
single catch-all `_server->on("/*", HTTP_GET, …)` that looks up the
asset table in flash and serves directly. Saves ~6-10 KB scattered
nodes but adds dispatch logic in the handler. Defer until 1-4 prove
insufficient.

## Acceptance criteria

After phases 0-4:
- `max_alloc` after first TLS handshake: target ≥ **40 KB** (was 9 KB)
- `min_free_ever` after 30-min soak with browser open: target ≥ **30 KB** (was 0.86 KB)
- `/rest/uiManifest` returns full payload on EVERY refresh (no 503)
- WG up/down cycle does not regress (already measured at +1.6 KB cost)

## Operational rules

1. **No code change without first running phase 0 instrumentation** and capturing the actual numbers.
2. **One phase per commit** — easy bisect if a phase regresses.
3. After each phase: rebuild + flash + 5-min smoke + compare numbers
   against the baseline. Reject any phase that doesn't improve
   `max_alloc` or `min_free_ever`.
4. Manifest cache approach changes (Phase 3) only AFTER phase 1+2
   land — otherwise the gains get masked by the existing holes.

## Open questions

1. Is `WiFi.mode(WIFI_AP_STA)` consuming 30 KB or is it less? Phase 0
   `boot:wifi-mode-set` will tell us. If WiFi alone is 50+ KB then
   no amount of framework cuts saves us — we'd have to question the
   C3 as a target board.
2. Does arduino-esp32 release any of the WiFi memory when only STA
   mode is active (no AP)? If so, switching to `WIFI_STA` at runtime
   after provisioning could reclaim significant heap.
3. ConfigManager — can we hold the shared JsonDocument in BSS via
   `StaticJsonDocument`? With ~4 KB needed that's borderline for BSS
   on C3 (40 KB BSS budget) but worth testing.

## Roll-back

Each phase is one commit. No phase depends on the next; any can be
reverted independently.
