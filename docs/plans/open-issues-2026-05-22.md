# Open issues — end of 2026-05-22 session

Snapshot of known problems left over after the TRIAL-enroll +
heap-audit + Serial→log marathon. Owner picks this up next session
and works top-down.

---

## 1. Tunnel doesn't close on Close-button request

**Symptom (operator-reported).** Operator clicks **Close** on the
device-panel; tunnel stays up (or the panel keeps saying it's up).

**Server side** (`app/routes/mship_ui_tunnel.py:243`):
```python
@app.route('/mothership/devices/<device_id>/tunnel/close', methods=['POST'])
def mship_tunnel_close(device_id):
    action = {"type": "closeTunnel", "params": {}}
    device_api.queue_action(device_id, action)
    _close_session(device_id)
    return jsonify({"ok": True, "reqId": action["reqId"]})
```

Action queued → delivered on next `/checkin` (≤ 5 s in active
panel session). Server side looks healthy.

**Device side** (`modules/mothership/src/MothershipService.cpp:856`,
then `modules/wireguard/src/WireguardService.cpp:432`):
```cpp
void WireguardService::down() {
  if (g_wg.is_initialized()) {
    g_wg.end();
    log_d("[wg] down: tunnel stopped");
  }
  update([](WireguardSettings& s) {
    s.is_up = false; ...
  }, "wg.down");
}
```

`WireguardService::loop()` immediately afterwards re-syncs from
the library:
```cpp
bool actually_up = g_wg.is_initialized();
if (actually_up != _state.is_up) { ...flip... }
```

**Three likely failure modes** (need serial to pin down):

| # | Hypothesis | Diagnostic |
|---|---|---|
| 1a | `g_wg.end()` returns but `is_initialized()` stays true on the francescolavra fork — next `loop()` tick flips `s.is_up` back to true → next `/checkin` reports `is_up=true` → panel stays green | Add `log_w` after `end()` showing both `is_initialized()` before/after, plus the lwIP netif walk to verify the "wg" netif actually went away |
| 1b | `_wg` is `nullptr` in `actionCloseTunnel` (cert-manager's late-bind via `setWireguardProvider` ran after MothershipService::begin captured a stale ptr) | Add `log_e("[mship.close] _wg=%p", _wg)` at the top of `actionCloseTunnel` |
| 1c | `g_wg.end()` works but the server-side peer is still in `wg.exe`'s active set, so when the device wakes the WiFi the kernel auto-reconnects through cached route entries before our `down()` is called | Check `wg show wg0` on the server before vs. after the operator click; check if route table has 10.99.0.0/24 entries surviving `g_wg.end()` |

**Action plan.**
1. Add diagnostic logs to `WireguardService::down()` + `actionCloseTunnel` (one commit, just for telemetry).
2. Reflash, click Close, capture serial.
3. Pick the right fix once we know which arm fired:
   * **1a**: stop trusting `is_initialized()` after `end()`. Force `s.is_up = false` and ignore the lib's view for ~5 s post-down; the netif walk in `loop()` is the source of truth for handshake/Tx/Rx.
   * **1b**: capture `_wg` defensively at action-dispatch time, or move dispatch into `WireguardService` itself via an action-registration callback.
   * **1c**: explicitly remove the route entries before `end()` returns, or accept it and document.

---

## 2. fwVer / hwRev / IP empty on device-panel until first check-in

**Symptom.** Device-panel header shows `(unknown)` for both fields
until the first mTLS check-in lands (~30 s by default).

**Root cause.** Device's `cert-manager` `postEnrollOnce` POST body
includes only `{deviceId, csr_pem [, wg_pubkey]}`. Server-side
`_issue_cert` now passes `fw_ver/hw_rev` to `record_device_seen`
(committed in `f17032b`), but the client doesn't send them yet —
`record_device_seen` falls through with empty strings.

**Action plan.** Add `fwVer`, `hwRev`, `hwUid` to the enroll body
in `CertManagerService::postEnrollOnce` (`modules/cert-manager/src/
CertManagerService.cpp` around line 990). Source values from
`DeviceIdentity::version()` / `DeviceIdentity::hwRevision()` /
`DeviceIdentity::macHex12()` (or `hwSuffix()`). Requires reflash.
Once landed, fresh trial enrolls show meta in the panel immediately.

---

## 3. Heap fragmentation — mbedtls memory arena (option 3)

**Symptom.** On ESP32-C3 after ~17 min of normal operation,
`mbedtls_ssl_setup` fails with `MBEDTLS_ERR_SSL_ALLOC_FAILED`
(-32512). Free heap stays ~55 KB but max-contiguous-alloc drifts
to ~21 KB — below the ~25 KB the default 16/16 KB record buffers
need.

**Current mitigations (already shipped).**
* `secureClient.stop()` after `http.end()` to force scratch back
  to heap immediately rather than waiting on HTTP/1.1 keep-alive.
* `log_i` heap-pre / `log_d` heap-post around the enroll handshake
  for live telemetry.

**Hard fix (this work).** Pre-allocate a fixed mbedtls arena at
boot, before WiFi/AsyncWebServer fragment anything:

```cpp
// In App::begin(), as early as possible
#include <mbedtls/memory_buffer_alloc.h>
static uint8_t s_mbed_arena[40 * 1024];
mbedtls_memory_buffer_alloc_init(s_mbed_arena, sizeof(s_mbed_arena));
```

After that, all mbedtls allocations come from the fixed arena.
Other heap users can't fragment it, the arena's free list is
internally compacted, and SSL handshakes are stable for the
device's lifetime.

**Risks / unknowns.**
* `MBEDTLS_MEMORY_BUFFER_ALLOC_C` may not be enabled in arduino-
  esp32's pre-built mbedtls (check `sdkconfig.h`). If not, fallback
  to `mbedtls_platform_set_calloc_free()` with a bump-allocator
  wrapping a static 40 KB buffer.
* 40 KB up-front cost is significant on C3 (288 KB total internal
  RAM). Worth it for stability, but profile after install.
* PSRAM-bearing chips (S3-N16R8, S3-N8R2) should route the arena
  into PSRAM via `heap_caps_malloc(MALLOC_CAP_SPIRAM)` — adds a
  small abstraction layer.

**Action plan.**
1. Verify `MBEDTLS_MEMORY_BUFFER_ALLOC_C` availability in current
   framework: try `#include <mbedtls/memory_buffer_alloc.h>` in a
   throwaway file, build C3, see if it links.
2. If yes: install in `App::begin()` with a 40 KB internal-RAM arena
   on C3 / C6 and a 64 KB PSRAM-routed arena on S3.
3. If no: install `mbedtls_platform_set_calloc_free()` hooks with
   our own bump-allocator (drops support for `free()` reuse, fine
   for short-lived handshakes that re-init the whole arena per call —
   or alternatively a freelist allocator).
4. Drop the per-call `secureClient.stop()` (no longer load-bearing).
5. Run the 17-min repro — must survive overnight cleanly.

---

## 4. Trial cert + device wipes its cert: re-enroll → new trial

**Status.** Currently `set_trial` is idempotent — same device re-
enrolling refreshes the existing trial entry with a new cert serial
+ new 30-day expiry. Means a forgetful operator effectively gets
infinite trial renewals via factory-reset of the device.

**Whether this is wrong depends on policy.** User in this session
implied factory-reset = new trial cycle, which matches current
behaviour. **Document as intentional** unless we want to add a
"once per device, ever" mode (would need to remember black-listed
deviceIds after first trial → first promote-or-bust outcome).

**Action plan.** Just write down the semantics in a short
`docs/trial-cert-semantics.md` so the policy isn't a hidden
implicit-pattern.

---

## 5. PENDING_ENROLLMENTS legacy table still exists on the dashboard

**Status.** Path 6 of `_handle_enroll` no longer parks into
PENDING_ENROLLMENTS — auto-issues a trial cert instead. The
dict + the dashboard table + the
`/mothership/enrollments/<id>/approve` route still exist as dead
code paths. No new entries land there.

**Action plan.** Two options:
1. Drop the table + route + dict entirely (cleanest).
2. Keep them as a "force-pending" mode the operator could opt
   into per-deviceId via blacklist-style gates (e.g. add a
   `force_review` setting that flips path 6 back to PENDING for
   future devices).

Recommend (1) unless we discover an operator wants (2). Three-line
patch to `state.py` + `device_api.py` + dashboard + the two route
handlers.

---

## 6. cert-enroll keeps polling when mothership module is "off"

**Status.** Conceptually decided in this session: `enabled` toggle
retired (already shipped). With it gone the question is moot —
mothership is always on if compiled in.

**Followup.** Audit any operator UX that referenced the toggle
(none currently, but `feature.mothership` build-flag still gates
inclusion at compile time).

---

## 7. Trusted-list left over from previous device

**Status.** `TRUSTED_DEVICES` on the server kept an entry for
`ESPRackDemo-d0cf1318f430-b67ba0d4` (an old physical chip the
user no longer uses). Harmless but clutters the table.

**Action plan.** Operator hits **Untrust** in the UI when they
notice. Not a code fix.

---

## Done in this session — for completeness

* UI manifest fetch race + stale-JWT clear → shipped `4eceb37`
* HttpEndpoint OOM guard (503 on alloc-fail) + buffer right-sizing
  across all features → shipped `6cce62c`
* 259 `Serial.print*` calls migrated to arduino-esp32 `log_*` via
  `scripts/convert_serial_to_log.py` → shipped `6cce62c`
* TRIAL_DEVICES on server + Promote/Revoke endpoints + UI section
  → shipped `f17032b`
* Mothership `enabled` toggle retired → shipped `6cce62c`
* Profiles tab folded into Settings → shipped `6cce62c`
* Cert-manager bootstrap-token persisted (factory-token flow) +
  5-state EnrollResult + persistent enrollment task → shipped
  `6cce62c`
* `start.bat` for Windows dev launcher → shipped `f17032b`

---

## Suggested next-session order

1. **Diagnostic logs for tunnel-close** (Section 1, item before fix).
   Quick reflash, capture, decide root cause.
2. **fwVer/hwRev in enroll body** (Section 2). Cheap, immediately
   visible win on operator panel.
3. **mbedtls memory arena** (Section 3). Big stability win; one
   investigation step + one implementation step.
4. **PENDING_ENROLLMENTS cleanup** (Section 5). Cleanup churn.

Items 4 / 6 / 7 are documentation chores; do whenever.
