# Memory-fragmentation master plan

## Why the previous attempts failed (and what we learned)

| Attempt | Mechanism | Why it broke |
|---|---|---|
| MbedtlsArena | Custom calloc/free hooks over a 40 KB BSS arena | First-fit allocator corrupted mbedtls's parsed CA-chain pointers mid-verify (-9984). Custom allocators are too easy to ship with subtle bugs at this maturity level. |
| TlsHeapReserve | `heap_caps_malloc(40K)` at boot, RAII `Lease` release-before / reacquire-after | The released block is NOT reserved for mbedtls. Between `Lease` ctor and `mbedtls_ssl_setup`, other tasks (AsyncTCP, lwIP, AsyncMqttClient, cert-parsing inside `attachToClient`) punch small allocations into the just-freed 40 KB region. By the time mbedtls asks for two 16 KB record buffers, the contiguous max has already dropped below the threshold. There's no race-free way to "hand off" a freed block to a specific allocator on a shared multi-task heap. |
| Soft-watchdog | `esp_restart` after N consecutive failures | Mask, not fix. Reset clock incompatible with years-uptime certification. |

**The lesson**: ANY strategy that involves "allocate-then-free-then-hope-mbedtls-grabs-the-right-region" is fragile by design. Multi-task heap allocators provide no mechanism to bind a freed block back to a specific consumer.

The same failure mode WILL eventually surface on every target the framework runs on. ESP32-S3 with PSRAM just buys orders of magnitude more time before the cliff — it doesn't move the cliff.

## The single architectural principle that fixes it

> **mbedtls allocations live for the device's lifetime. They are made once, at boot, when heap is fresh, and are never freed.**

If mbedtls never frees its big buffers, fragmentation around them is irrelevant — those 32 KB are out of the freelist forever, the system heap fragments however it wants, and TLS handshakes always have the structures they need.

Everything below is the implementation of that principle.

---

## Phase 1 — Persistent TLS client instances *(highest impact)*

### Goal
Each module that opens HTTPS connections holds ONE `WiFiClientSecure` instance as a class member. Allocated at boot (or on first network event after WiFi up), reused for every subsequent HTTP request. The underlying mbedtls SSL context is created on first connect and held across reconnects.

### Affected modules
- `CertManagerService` — `postEnrollOnce`, `postCsrToRenew`, `postRecoveryRequest`. Three call sites today; merge to one.
- `MothershipService` — `performOneCheckin`. One call site.
- `AutoUpdateService` — HTTP fetch of OTA payload (currently HTTP not HTTPS, but the pattern generalises).
- `TelegramService` — Bot API requires HTTPS to api.telegram.org.

### Implementation
1. Add an `ITLSClient` provider in `lib/ESPRack/include/`. Owns a `WiFiClientSecure` lazily allocated on first use, with the framework CA chain + device cert/key pre-attached via existing `ITLSProvider`. Methods:
   - `WiFiClientSecure& client()` — returns the singleton.
   - `bool ensureConnected(const String& host, uint16_t port)` — opens or verifies the TCP+TLS connection; reconnects without destroying the mbedtls SSL context.
   - `void disconnect()` — closes the socket but PRESERVES the SSL session for resumption.
2. `App` exposes `ITLSClient* tlsClient()` so any service can grab the shared instance. Services that wrap `HTTPClient` use `http.begin(app->tlsClient()->client(), url)` and `http.setReuse(true)`.
3. Wrap `WiFiClientSecure::stop()` to NOT call mbedtls free. We override behaviour by NEVER calling `client.stop()` — only `http.end()` which preserves the underlying client when `setReuse(true)`.

### Server-side prerequisite
The mothership HTTPS device-API (`app/mothership/server_thread.py`) must support HTTP keep-alive. `ThreadingHTTPServer` does by default in HTTP/1.1; we need to verify the response includes `Connection: keep-alive` and the client picks it up. **Action**: set `BaseHTTPRequestHandler.protocol_version = "HTTP/1.1"` and ensure no explicit `Connection: close` in `_send_json`.

### Acceptance
- Boot serial shows ONE `[tls-client] handshake` log line for each persistent client, then the line never repeats during a 30-day soak.
- Free-heap max-contiguous-alloc, measured at every checkin, stays within a 2 KB band around its boot-time value forever.

### Estimated effort
2-3 dev-days for the framework changes + audit of all call sites. Server-side keep-alive: 30 min.

---

## Phase 2 — Pool-allocated `ArduinoJson` documents *(medium impact)*

### Goal
The `DynamicJsonDocument req(2048)` / `(4096)` / `(8192)` patterns are scattered throughout cert-manager / mothership / etc. Each construction does a heap alloc; destruction frees. Per-checkin we allocate + free ~6 KB of these. Over a year that's ~50 million alloc/free cycles, each with small fragmentation potential.

### Implementation
1. `lib/ESPRack/include/JsonPool.h` — singleton pool with fixed-size slots:
   - 4× 2 KB slots
   - 4× 4 KB slots
   - 2× 8 KB slots
   - Slots allocated in BSS (`alignas(8) uint8_t pool_4k[4][4096]`). Zero-cost heap-wise.
2. Replace every `DynamicJsonDocument doc(N);` with `auto doc = JsonPool::acquire(N);` returning a RAII wrapper that releases the slot on destruction.
3. Audit: only ~20 call sites. Strict slot sizing means anything over 8 KB needs special handling (fall through to heap with a warning log).

### Acceptance
- No `DynamicJsonDocument` constructions in framework or modules after migration (lint check via grep).
- Heap free count stays steady — JsonPool acquires don't show up as malloc/free pairs in `heap_caps_get_allocated_size` deltas.

### Estimated effort
1-2 dev-days. Mechanical conversion + tests.

---

## Phase 3 — Replace Arduino `String` in hot paths *(low impact, high cumulative)*

### Goal
`String` doubles its buffer on grow, frees the old one. Concat patterns like `String url = base + "/api/v1/" + endpoint;` allocate ~5 intermediate buffers. Every checkin we do dozens of these. Same fragmentation contributor as ArduinoJson docs.

### Implementation
1. Identify hot paths via grep + serial profiling. Top candidates:
   - URL construction in `effectiveEnrollUrl()` / `effectiveRecoverUrl()` / `MothershipSettings::checkinUrl()`.
   - Status label assignment in tick handlers.
   - Cert PEM concatenation (rare but big).
2. Replace with `char buf[N]; snprintf(buf, sizeof(buf), ...)` where the upper bound is known.
3. For dynamic-size cases, use a thread-local `String` of pre-grown capacity (`scratch.reserve(256)` once at task start, reuse across iterations).

### Acceptance
- Per-checkin String constructs counted via instrumentation drop from ~30 to ≤ 3.

### Estimated effort
2 dev-days for audit + replacement.

---

## Phase 4 — Heap-instrumentation service *(low effort, enables prevention)*

### Goal
Continuous visibility into heap state so we catch degradation BEFORE it crosses the OOM threshold. Lets the operator see "device's max-contig has been drifting down for the last hour, something is leaking" without needing a serial monitor.

### Implementation
1. New `HeapMonitorService` (priority high — installs early):
   - `loop()` once per minute: read `ESP.getFreeHeap()`, `ESP.getMaxAllocHeap()`, `ESP.getMinFreeHeap()`.
   - Maintain a ring of the last N samples (60 = one hour at 1-min cadence).
   - Compute moving average + drift slope.
2. Expose a "Heap" tab on the System feature with:
   - Current free / max-alloc / min-ever-seen.
   - Sparkline of last hour.
   - Slope warning: if max-alloc has dropped > 10% in the last hour, badge it red.
3. Mothership `/checkin` body grows a `heap` block carrying current + min + drift. Server-side fleet dashboard shows aggregate fleet-health.

### Acceptance
- A fragmenting device is visible to the operator in ≤ 5 minutes after degradation starts, instead of waiting for the device to fail.

### Estimated effort
1 dev-day.

---

## Phase 5 — Single-task TLS dispatcher *(high impact, high effort — only if Phase 1-4 don't fully close the gap)*

### Goal
Funnel ALL TLS traffic through a single dispatcher task with a request queue. Eliminates concurrent TLS handshakes (cert-manager + mothership simultaneously) which double the peak heap footprint.

### Implementation
1. `lib/ESPRack/src/TlsDispatcher.cpp` — owns one persistent `WiFiClientSecure` and one HTTPClient. Exposes:
   - `QueueHandle_t tx_queue` — services post `TlsRequest` items (method, url, body, callback).
   - Worker task pops items, executes, fires callback with response.
2. Migrate cert-manager + mothership + auto-update + telegram to post to the queue instead of opening their own clients.
3. Single mbedtls allocation footprint for the entire device. TLS state outlives every individual request.

### Acceptance
- Peak heap usage during any 24 h period stays within 4 KB of boot value.
- TLS handshake count over a 24 h period: ≤ 5 (only on initial bring-up + WiFi disconnects).

### Estimated effort
4-5 dev-days. Major refactor but biggest single win.

---

## Phase 6 — Soak-test methodology *(prerequisite for certification)*

### Goal
Reproducible evidence that the device runs N days continuously without degradation. Documented procedure that ships with the cert package.

### Implementation
1. A test harness device flashed with instrumented firmware that logs every minute to a local file (LittleFS or SD) AND to the mothership.
2. Soak duration tiers:
   - **Smoke**: 6 h (catches obvious leaks)
   - **Standard**: 7 days (catches slow leaks)
   - **Certified**: 90 days (matches typical IoT cert windows)
3. Acceptance metrics:
   - Free heap delta from boot to end: ≤ 5 KB (allows for normal cache fill).
   - Max-alloc never drops below 30 KB on C3 / 60 KB on S3.
   - Zero TLS handshake failures over the soak window.
   - Mothership checkin success rate ≥ 99.9%.
4. Failure post-mortem: every soak failure produces a serial-log dump + heap-trace + the operating-condition delta from the previous successful soak. Tracked in `docs/soak-results/`.

### Estimated effort
1 dev-day for harness, plus calendar time for the soaks themselves.

---

## Suggested execution order

**Sprint 1 (1-2 weeks)**: Phase 4 (instrumentation) + Phase 1 (persistent TLS clients). Together these surface the problem and address its single biggest contributor. After this sprint, do a 7-day soak. If pass → ship and continue with smaller phases for hardening.

**Sprint 2 (1 week)**: Phase 2 (JsonPool). Eliminates the second-largest fragmentation contributor.

**Sprint 3 (3-4 days)**: Phase 3 (String hot paths). Cleanup, small wins.

**Sprint 4 (only if needed)**: Phase 5 (TLS dispatcher). Heavy refactor but ultimate solution if Phase 1 alone doesn't hold across 90-day soak.

**Always-on**: Phase 6 soak methodology runs alongside every phase.

---

## What's NOT in this plan (and why)

- **Custom `mbedtls_platform_set_calloc_free` hooks** — already tried (MbedtlsArena). Implementing a robust custom allocator is harder than reusing existing allocations. Out.
- **Framework rebuild with smaller `MBEDTLS_SSL_IN/OUT_CONTENT_LEN`** — already attempted (pioarduino `custom_sdkconfig`). Pioarduino's framework-rebuild path is fragile across PIO installs and adds 10+ minutes per build for every team member. Better to fix the lifecycle (this plan) than fight the toolchain. Out.
- **Auto-reboot watchdog** — rejected. Reset clock incompatible with certified years-uptime.
- **Switch to ESP32-S3 hardware** — postpones the problem rather than solving it. Still worth pursuing in parallel for SKU diversification, but not a substitute for the lifecycle fix.

---

## Open questions for the build / cert team

1. Does the certification process accept "first 60 s after boot has TLS handshakes; after that, zero handshakes for years" as a memory-stability proof? Or do we need to show that EACH handshake during steady-state also succeeds?
2. Is the server-side keep-alive change (HTTP/1.1 in the mothership device-API) acceptable security-wise, or do we need TLS session resumption with explicit ticket lifetime caps?
3. Do we have a hardware-in-the-loop soak rig available, or do we need to build one as part of Phase 6?
