# Audit: feature/mship-ui-direct-monitor → feature/wireguard-tunnel

Comprehensive comparison of the abandoned HTTP-tunnel branch against
the new WireGuard-tunnel branch. Goal: identify every improvement on
the old branch that's worth carrying forward, minus the HTTP-tunnel
machinery itself (which is replaced by on-demand WireGuard).

## TL;DR

- **49 commits** total on the old branch ahead of WG-tunnel.
- **15 commits** already migrated (manually re-created or
  cherry-picked).
- **13 commits** are HTTP-tunnel dead-end — skip permanently.
- **21 commits** are migration candidates — categorised by tier
  below.

Most of the high-value architecture work was Phase 1-6 of the
mothership roadmap (action queue + rest.proxy + setCadence +
manifest_rev + config backup + cert rotation/recovery) plus the
DeviceIdentity refactor. Only Phase 7 was HTTP-tunnel-specific.

---

## Already migrated (no action)

| Commit | Title | Migrated as |
|---|---|---|
| `47e8b50` | DeviceIdentity helper as canonical id source; drop ESP8266 | `1de2180` |
| `c576616` | HW-suffix expansion, fw tags, MdnsModule + Identity tab | `1de2180` (mdns part deliberately skipped) |
| `73e0968` | static form — no loops, no conditionals, no derived values | `de087e0` |
| `e487005` | two-slot profiles with dropdown + PKI URL fallback | `e6212ce` |
| `c263618` | cadence in seconds + auto-migrate legacy interval_min config | `7f2aad1` |
| `a0facdf` | use literal keys for profile slots | superseded by `de087e0` |
| `3e23e6e` | REST-only feature + 16K buffer | superseded by `de087e0` |
| `4e75756` | drop help messages, ASCII-only dropdown | superseded by `de087e0` |
| `d3caf86` | auto-enroll loop with deviceId | `6e89a76` |

---

## HTTP-tunnel dead-end (skip permanently)

These exist only because the HTTP-tunnel transport had structural
problems on ESP32 heap. WG.4 replaces the entire transport — every
fix below disappears with it.

esp-rack:
- `3122aa4`, `2bc4ef5`, `86a47b8`, `747bb84`, `5cc2d48`,
  `085968d`, `1833f9a`, `56f1597`, `cfd5cfb`, `499c6ab` — all
  `mship-bridge` module fixes (TLS slot, queue, settle window,
  MinimalWsClient, stack bumps) → mship-bridge module itself doesn't
  exist on WG-tunnel branch.
- `8be94ed`, `d4727f2`, `1eda1f1` — `ui.fetch.bulk` / `ui.fetch`
  pipeline → replaced by `mship_ui_tunnel.py` reverse-proxy.

esp-rack-light-demo (4 commits):
- `00451d4`, `83843c0`, `63a2dad`, `3fab6d2` — MshipBridgeModule
  install/disable.

esp-update-server-master (15 commits):
- `369e518`, `ef15841`, `a165005`, `c6ac5ae`, `9b0e868`,
  `c2a08ad`, `94ef525`, `d193254`, `08256d0`, `013e5cc`, `9cc13f2`,
  `d25c64e`, `4c3b2e8` — `device_ui.py`, `ws_bridge.py`,
  `asset-upload`, manifest-cache, SPA-shim, BULK pipeline.
- `0b824fa` (Phase 7d WS bridge plan doc) — archive-only, no migration.
- `a8fbbf4` (urllib over requests) — already adopted in WG.4
  proxy by convention.

---

## Migration candidates — Tier 1 (high priority, bug fixes)

These are operator-facing bugs we'll hit in production. Land soon.

### `c7e3cff fix(cert): NTP-race made fresh certs look GrayZone forever`
Cert-manager's GrayZone detection compares `not_after_ts` against
the system clock. On a cold boot before NTP sync, `time(nullptr)` is
~UNIX epoch 0, so a freshly-enrolled cert (notAfter = now + 90 d)
looks expired by 50+ years. Fix gates the comparison on a "clock
trustworthy" check (year >= 2024).

**Migration risk:** Low. Single function change in
CertManagerService::refreshRuntimeState. Cherry-pick clean.

### `b620079 fix(cert.recover): re-click Trigger Recovery wakes the polling task`
Operator clicks Recover → polling task spawns → recovery succeeds /
fails → task exits. If operator clicks Recover again (e.g. after
fixing a network issue), nothing happens because the task handle
isn't reset. Fix nulls the handle on exit.

**Migration risk:** Low. Depends on Phase 4b recovery code
(`30d7519`) — bring both together.

### `0e6bb90 fix(mship): reboot action — esp_restart() inline, full HW flavor in checkin`
Two fixes in one commit:
1. `actionReboot` was spawning a 2-second-delayed FreeRTOS task that
   sometimes deadlocked with the check-in task holding the mbedtls
   slot. Fix calls `esp_restart()` inline after pushing the result.
2. Check-in body's `hwVer` was the bare `getChipModel()` ("ESP32-S3")
   instead of the full flavor ("ESP32-S3-N16R8V"). Partially
   addressed by my `1de2180` DeviceIdentity wiring — the inline
   esp_restart half still needs to land.

**Migration risk:** Low. One function change.

### `3c6106f fix(fs): drop volatile++ deprecation warning on _formatDone`
arduino-esp32 v3 deprecated `volatile T operator++(int)`. Fix uses
explicit `_formatDone = _formatDone + 1`. Cosmetic but eliminates
build noise.

**Migration risk:** Trivial. Cherry-pick clean.

---

## Migration candidates — Tier 2 (must-have features)

### `cb44c16 feat(cert+mothership+mock): Phase 4a — proactive cert rotation`
Device monitors its own cert lifetime; when remaining days drop
below `renew_threshold_days` (server-side setting we already
migrated), kicks `actionRenewCert` flow → CSR → server signs new
cert → device atomic-swaps → no operator action needed.

Pairs cleanly with the per-device cert validity work in `4adf6c7`
(if operator sets 30-day certs, rotation kicks in at ~25 days).

**Migration risk:** Medium. Touches cert-manager + mothership
action dispatcher + server `_handle_renew` route. Server-side route
already on master/wireguard-tunnel base — only device-side renewal
trigger + result handler needs to land.

### `30d7519 feat(cert+mock): Phase 4b — gray-zone recovery (operator-approved re-issue)`
When cert expires while device was offline (cold-storage / power
loss), normal renew fails (mTLS rejected by server). Recovery flow:
device POSTs `/api/v1/recover` with recovery_token instead of
client cert → server parks in `PENDING_RECOVERY` → operator clicks
Approve on /mothership → device's next poll drains the signed cert.

Server-side already on master (it's Phase 1 work that landed before
the WG-tunnel cutoff). Device-side polling + state-machine needs to
land.

**Migration risk:** Medium. Self-contained recovery flow, ~200
lines on device. `b620079` is the matching bug fix.

---

## Migration candidates — Tier 3 (action infrastructure)

These are the building blocks Phase 7 sat on top of. WITHOUT the
HTTP-tunnel parts, they're still useful for OPERATOR-via-mothership
flows: queueing config changes, scheduling reboots, fetching device
state from the dashboard.

### Action queue + result channel
- `80fc2a1 feat(mship.phase1): action-result return channel + reqId`
- `0d00c24 fix(mship): chunk large action results across check-ins instead of bumping buffers`
- `01bf34f fix(mship): drop setCadence acks from ring — fire-and-forget action`

Server queues `{action, reqId}` → device drains on check-in →
executes → returns `{reqId, status, summary}` back on the next
check-in. The chunking commit handles >1 KB results without
blowing the 1 KB request body limit.

**Migration risk:** Medium. Self-contained in MothershipService.
Server side already supports it (Phase 1 commits pre-date the WG
cutoff).

### REST proxy + manifest emission
- `523564a feat(mship.phase2): generic rest.proxy via WebManager::proxyDispatch`
- `c5fcfdb feat(mship.phase5): manifest reachable via rest.proxy + device emits manifest_rev`
- `b156ecb feat(http-endpoint): registerProxy() so HttpEndpoint-mounted REST is mship-ui reachable`
- `9163e86 fix(build): break HttpEndpoint<->WebManager include cycle`
- `3c0143a feat(mship): generic proxy-endpoint registry + wire FeaturesService into it`
- `552dc8e feat(mship-ui): registerProxy sweep`

Operator can hit `/mothership/devices/<id>/proxy` with
`{method, path, body}` — server queues a `rest.proxy` action →
device executes against its own WebManager → result back on
check-in. Useful as a SLOW fallback when WG tunnel isn't up
(e.g. device offline mid-session, operator just wants to schedule
a config push).

**Migration risk:** Medium-high. `b156ecb` was the include-cycle
fix that broke the build initially; `9163e86` followed. Need to
cherry-pick both. The `registerProxy sweep` touches every existing
module's manifest registration.

### Cadence override
- `4e37805 feat(mship.phase3): active-session cadence override via setCadence`

Already used by the WG-tunnel flow (mship_ui_tunnel.py sends
`setCadence period_s=5 ttl_s=300` to bump polling when operator
opens a panel). Device-side handler doesn't exist yet on this branch.

**Migration risk:** Low. ~60 lines in MothershipService::dispatchActions.

### Config backup
- `3821e93 feat(mship.phase6): config dumpAll/restoreAll + bulk backup actions`

Mothership can snapshot a device's entire `/config/*.json` tree and
restore on demand. Useful for operator-side fleet management:
"snapshot device A → push snapshot to fresh device B" workflow.

**Migration risk:** Low-medium. Self-contained `actionConfigDump` /
`actionConfigRestore` in MothershipService.

### Dynamic check-in sleep
- `1a57de9 fix(mship): dynamic check-in sleep + wire /rest/signIn through proxy registry`

Check-in task computes its next-sleep dynamically from
`base_interval_s` / `burst_interval_s` / `cadence_override`. Replaces
the fixed 5-second `vTaskDelay`. Already partially landed via my
static-form work; this commit is the canonical reference.

**Migration risk:** Low. Inspect against current
`MothershipService::runCheckinLoop` and pick up missing nuances.

### Phase 4 refactor
- `e211c54 refactor(phase4): all raw server->on() routes now flow through WebManager`

Mass-refactor: every module that registered raw `server->on(path,
handler)` switches to `WebManager::registerHttpEndpoint`. Benefits
proxying (`b156ecb` depends on it) AND operator-side auth (every
endpoint goes through SecurityManager).

**Migration risk:** High. Touches 8+ modules. If we want
`registerProxy` to work, this is a prerequisite. If we don't —
skip.

### UI fixes (frontend)
- `10a8d65 fix(ui): action lookup also scans top-level kind='action' entries`
- `330969a feat(ui): EndpointMissing fallback for widgets whose backend isn't installed`

The first is a 5-line lookup bug fix. The second is a graceful
"this widget's REST endpoint isn't installed in your build" empty
state instead of an infinite spinner.

**Migration risk:** Low. Cherry-pick clean (TypeScript).

### `974be6f fix(system-status): migrate Identity/Resources tabs to WebManager + internalHandler`
Likely already covered by my `1de2180` cherry-pick of `c576616`'s
system-status update. Verify before re-applying.

---

## Recommended migration order

Tier 1 first (small surface, real bugs):
1. `c7e3cff` NTP-race GrayZone fix
2. `3c6106f` volatile++ warning fix
3. `0e6bb90` reboot-inline (HW flavor half already done)

Tier 2 (cert lifecycle completion — pairs with cert-validity work):
4. `cb44c16` Phase 4a proactive rotation
5. `30d7519` Phase 4b gray-zone recovery
6. `b620079` recovery re-click fix (depends on 5)

Tier 3 only if/when a use case demands it:
7. `4e37805` setCadence handler (needed for WG.4's
   `_ensure_active_cadence` to actually do something on device side
   — currently the cadence override action is sent but device
   ignores it). **Recommend doing alongside WG.4 first real test.**
8. `80fc2a1` + `0d00c24` + `01bf34f` action-result channel + chunking
   (needed before any operator-side `rest.proxy` works as fallback).
9. `523564a` + `b156ecb` + `9163e86` + `3c0143a` + `552dc8e` REST
   proxy stack — only after tier 3.8 lands. ALSO requires `e211c54`
   refactor first.
10. `3821e93` config backup — independent, low priority.
11. `e211c54` raw-route refactor — only if doing rest.proxy stack.
12. UI fixes `10a8d65` + `330969a` — anytime, low risk.

---

## Things NOT to migrate (recap)

- Anything `mship-bridge` (the module itself, all its fixes).
- Anything `ui.fetch` / `ui.fetch.bulk` / `asset-upload`.
- Server-side `device_ui.py` / `ws_bridge.py`.
- The MshipBridgeModule install lines in demo's main.cpp.

All operator UI access to device REST goes through WG tunnel
proxy (`mship_ui_tunnel.py` + `/mship-ui/<id>/<path>` route from
WG.4). The mothership check-in queue is still useful for
asynchronous fleet ops (reboot, cert renewal, config push), but
NOT for interactive browser → device REST — that's the WG path.
