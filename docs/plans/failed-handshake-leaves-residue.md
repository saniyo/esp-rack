# Bug — failed TLS handshakes leave heap fragmentation residue

## Symptom

While running the v0.1.1 WireGuard-ESPRack soak with the
manifest-from-flash + Cache-Control + Connection: close batch, we
hit a stretch where the mothership server cert was misconfigured
(separately tracked at `esp-update-server-master/docs/plans/persisted-cert-stale-san.md`).
The device looped on `performOneCheckin → ensureConnected → BR err=0`
+ `hardReset(BearSSL)` for ~25 attempts (about 6 minutes) before
the operator fixed the server cert.

After the fix the next handshake succeeded and the device entered
the steady state we wanted to measure. But the post-warmup
snapshot showed:

```
post-warmup: free=36344 max_alloc=9204 min_free_ever=2948
```

`max_alloc=9204` is wrong. A clean-boot device with the same firmware
arrives at post-warmup with `max_alloc≈28-30 KB`. The ~20 KB delta
*came from the 25 failed handshake attempts*. Each failed attempt
left some residue — somewhere between client-side BearSSL state,
PersistentTlsClient bookkeeping, or lwIP socket teardown — that
didn't fully release on `hardReset`.

In a more general sense: **failed handshakes should be cheap**. A
bursty environment (e.g. tunnel side dropping packets, server
restarting, NAT eviction) can produce dozens of failures in minutes,
and the device must be able to clear that backpressure as the
connection comes back. Currently it can't — once heap is fragmented
to 9 KB max_alloc, BearSSL workspace allocations start failing on
their own and the device enters a slow-spiral state.

## What we expect

1. A failed `ensureConnected → TLS upgrade → hardReset` cycle should
   leave `max_alloc` within a few hundred bytes of where it started.
2. Subsequent retries reuse buffers — both TCP and TLS — instead of
   allocating fresh state per attempt.
3. After the first SUCCESSFUL handshake in a session, `max_alloc`
   should equal the value we'd see on a clean-boot device at the same
   point (post-warmup ≈ 28 KB on C3 with current framework).

## What's likely happening (hypotheses to test)

1. **BearSSL static buffers in BSS — but ALONGSIDE per-handshake
   heap allocations.** ESP_SSLClient's lifecycle creates a small set
   of state structs on the heap each handshake. Successive
   alloc/free leaves fragmentation residue even if the *bytes*
   balance out.
2. **`hardReset()` re-runs `loadCerts()` — which copies PEM blobs
   from ITLSProvider into ESP_SSLClient.** Each load might call
   into mbedtls / BearSSL parsers that touch heap. Repeated loads
   leave repeated holes.
3. **lwIP TCP teardown of the failed socket leaks state to a
   subsequent `tcp_new`.** Each connect failure + close cycle
   advances some pool index without compacting; net effect = drift.
4. **AsyncTCP send/recv queue residue** if the failed socket left
   data in a buffer that wasn't fully drained at close.

## Investigation plan

1. **Instrument `PersistentTlsClient::ensureConnected` and `hardReset`**
   with `HeapMonitor::logSnapshot` at:
   - method entry
   - just before `_ssl.connect(host, port)`
   - just before `_ssl.connectSSL()`
   - immediately after a failure return
   - inside `hardReset` before and after `_ssl.stop()` + `loadCerts()`
   This gives us per-step `max_alloc` deltas across a single failed
   cycle.
2. **Reproduce a controlled fail loop**: temporarily point the
   device at a TCP-listening peer that closes immediately
   (`nc -lk 9999 -c 'true'` style), so every handshake fails fast.
   Watch the snapshot deltas over 50 cycles.
3. **Identify the leak source** from the per-step deltas. Patch the
   smallest thing that produces a >0 max_alloc loss per cycle.

## Likely fixes (depending on what step 3 finds)

* **A. `loadCerts()` re-allocates PEM copies on every hardReset** —
  cache the parsed cert/key pointer set on first load, point
  ESP_SSLClient at them by reference rather than rebuilding from
  source each time.
* **B. `_ssl.connect()` opens a fresh `WiFiClient` per call** — pin
  the underlying `WiFiClient` instance on construction and reuse
  it; `_ssl.stop()` only closes the socket, not the wrapper.
* **C. lwIP `tcp_close` race** — when `_ssl.connect()` fails fast,
  the WiFiClient's underlying `tcp_pcb` might land in TIME_WAIT and
  never coalesce. Forcing `setNoDelay(true)` + `abort()` on failed
  connects releases the pcb immediately.
* **D. ESP_SSLClient lifecycle** — file an upstream bug if the
  library itself can't recycle state across failed handshakes. We
  may need a tiny vendor patch or switch to direct BearSSL bindings.

## Acceptance

Run 50 deliberately failed handshakes against a closing-peer port.
After each pair, compare `max_alloc` to the previous baseline:

* `Δmax_alloc per failed cycle ≤ 256 B` (essentially noise).
* `Δmax_alloc over 50 cycles ≤ 4 KB`.

If both hold, fragmentation is no longer accumulating from this
path.

## Why this matters

The PersistentTlsClient is the only piece of the framework that
talks to a remote service. If a server bounces, a cert rotates, or
a WAN link flutters, we MUST be able to fail-recover cleanly without
slow-spiralling heap into terminate territory. The handshake-leak
behaviour we observed during the server-cert-stale incident makes
the device much less resilient than its hardware would suggest.

This is the failure mode we kept chasing in earlier sessions
(mish-mash of "WG eats heap", "manifest cache fragments heap",
"ManifestBuilder bad_alloc") — all of those are *symptoms* of the
device starting from a heap-fragmented baseline rather than a clean
one. Fixing the failed-handshake residue moves the baseline back
where it should be.
