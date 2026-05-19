# Plan: On-Demand WireGuard Tunnel for Mothership-Device Access

## Context

We're picking up Phase 3 from
[mothership-roadmap.md](./mothership-roadmap.md) — the `openTunnel`
action which was always part of the design but never implemented —
and making the tunnel the **only** mechanism by which an operator
gets at a specific device's REST UI / asset bytes. This replaces the
entire Phase 7a-7e experiment that tried to emulate a tunnel inside
check-in HTTPS (`ui.fetch.bulk` + `mship-bridge` WS over mTLS). That
experiment is archived on `feature/mship-ui-direct-monitor`.

**Architectural decision (operator-confirmed):**

> Mothership does **not** go through the tunnel for normal operation.
> The tunnel is **occasionally used for direct access**. It is
> **raised on command** sent over the existing mTLS check-in channel.

So the model is:

- **Control plane**: existing `/api/v1/checkin` over mTLS HTTPS,
  unchanged. Server queues actions, device executes them.
- **Data plane (operator UI bytes)**: WireGuard tunnel, brought up
  on demand by a new `openTunnel` action when an operator opens a
  device's UI panel, brought back down by `closeTunnel` after the
  operator's session ends or after idle timeout.
- **Device's own REST API**: same AsyncWebServer on `:80` that
  already serves the LAN web UI. No "tunnel mode" branch in handlers
  — server just becomes reachable on a second NIC (the wg0 interface)
  while the tunnel is up.

This keeps the device idle-heap budget intact (only check-in TLS
exists between operator sessions), while sidestepping every
heap-pressure race we hit in Phase 7e (multiple concurrent mbedtls
contexts).

---

## Goals & non-goals

### Goals
- **On-demand tunnel** — up only while an operator is actively
  viewing a device's UI. Inactive devices in the fleet hold zero
  tunnel state.
- **`openTunnel` / `closeTunnel` actions** sent over the existing
  check-in channel, reusing the action dispatcher we already have.
- **Configurable endpoint** — mothership WG endpoint is a settings
  field (today `192.168.10.202:51820`, tomorrow a public IP / DDNS
  hostname).
- **Configurable subnet** — operator picks size (`/24`, `/16`, …)
  during initial setup, server allocates IPs from this pool.
- **PKI integration** — WG keypair generated alongside the X.509
  cert during enrollment, persisted via SecretsVault, revoked when
  the device cert is revoked.
- **Operator UX**: clicking "Open UI" on the mothership dashboard
  produces the device's real React UI within ~5-10 s (one check-in
  cycle + WG handshake), no chunked-asset cache, no streaming
  pipeline.

### Non-goals
- **Persistent tunnels** — explicitly NOT the model. If a device
  needs to push telemetry that the server consumes, it goes through
  check-in (existing) or a separate per-feature long-poll, not
  through WG.
- **Direct device-to-device WG mesh** — single-hop only, device ↔
  mothership.
- **Bypassing PKI** — enrollment of the WG peer rides the same
  bootstrap-token flow as the X.509 cert. No parallel auth.
- **Server-side WG implemented in pure Python** — we use the kernel
  WireGuard module via `wg-quick` / `wg syncconf` on the mothership
  host.

---

## End-to-end flow (the operator scenario)

1. Operator opens mothership dashboard, clicks "Open UI" on device
   `ESPRackDemo-…`.
2. Flask route `POST /mothership/devices/<id>/tunnel/open`:
   - Queues an `openTunnel` action with reqId for that device.
   - Marks the operator's session "tunnel pending".
   - Returns 202 to browser; browser shows "starting tunnel…"
     spinner.
3. Device's next check-in (≤ base interval, typically 5-60 s) picks
   up the action.
4. `actionOpenTunnel` handler:
   - Pulls server pubkey + assigned tunnel IP + endpoint from the
     params (cached from enrollment).
   - Calls `WireGuardModule::up()` — esp_wireguard creates the wg0
     netif, configures it, sends initial handshake.
   - Once `wg show` reports the handshake complete (device knows
     this from `wireguardif_peer_is_up`), pushes an `action_result`
     to the ring with status 200 `{"tunnel_ip": "10.99.0.7"}`.
5. Action result lands at server on next check-in (sub-second after
   handshake completes).
6. Flask flips the session from "tunnel pending" → "tunnel live",
   browser's spinner clears, page redirects to the device's UI.
7. Operator browses freely — every browser request to
   `http://mothership:5000/mship-ui/<id>/<path>` is proxied by
   Flask via `requests.get(f"http://10.99.0.7/{path}", ...)`. Plain
   HTTP over WG, no asset cache, no streaming pipeline.
8. Operator closes the tab. Browser stops sending requests.
9. After `tunnel_idle_timeout_s` (default 300 s) of no operator
   traffic to that device, Flask queues `closeTunnel`. Device's
   next check-in delivers it, `WireGuardModule::down()` tears wg0
   down. Free heap restored.

Failure modes:
- Step 4 handshake fails → device pushes `action_result` status 500,
  Flask surfaces "tunnel failed" to operator. (Likely cause:
  endpoint unreachable from device.)
- Step 7 connection refused mid-session → wg0 went down for some
  reason; Flask emits an "Open UI" link again, operator re-clicks
  to re-trigger `openTunnel`.

---

## Architecture (with tunnel up — i.e. operator session active)

```
                 ┌───────────────────────────────────────┐
                 │ operator's browser                    │
                 │ http://mothership:5000/mship-ui/      │
                 │      <deviceId>/<asset>               │
                 └───────────────┬───────────────────────┘
                                 │  HTTP
                                 ▼
            ┌───────────────────────────────────────────┐
            │ mothership host                           │
            │ ┌─────────────────────────────────────┐   │
            │ │ Flask :5000  (admin + mship-ui)     │   │
            │ │  ─ POST /tunnel/open ─ queue action │   │
            │ │  ─ /mship-ui/* ─ proxy via wg0      │   │
            │ │  ─ idle timer per tunnel            │   │
            │ │  ─ POST /tunnel/close ─ on idle     │   │
            │ └────────────┬────────────────────────┘   │
            │ ┌─────────────────────────────────────┐   │
            │ │ ALSO: existing :8443 mTLS API       │   │
            │ │  (/checkin, /enroll, /asset-...)    │   │
            │ │  — the CONTROL plane stays here     │   │
            │ └────────────┬────────────────────────┘   │
            │              │ HTTP proxy → 10.99.0.X:80  │
            │              ▼  (only when tunnel is up)  │
            │ ┌─────────────────────────────────────┐   │
            │ │ kernel wg0  10.99.0.1/SUBNET        │   │
            │ │ port 51820, peers added on enroll   │   │
            │ └────────────┬────────────────────────┘   │
            └──────────────┼────────────────────────────┘
                           │  UDP/51820 (encrypted)
                           ▼
            ┌───────────────────────────────────────────┐
            │ device (esp32-s3 / -c6 / …)               │
            │                                           │
            │ ┌─────────────────────────────────────┐   │
            │ │ MothershipService                   │   │
            │ │  ─ TLS check-in every N s           │   │
            │ │  ─ actionOpenTunnel ─ calls         │   │
            │ │    WireGuardModule::up()            │   │
            │ │  ─ actionCloseTunnel ─ down()       │   │
            │ └─────────────────────────────────────┘   │
            │                                           │
            │ ┌─────────────────────────────────────┐   │
            │ │ WireGuardModule (NEW)               │   │
            │ │  ─ esp_wireguard wg0 (only up       │   │
            │ │    while operator session active)   │   │
            │ │  ─ assigned IP from enrollment      │   │
            │ │  ─ peer = mothership pubkey         │   │
            │ └─────────────────────────────────────┘   │
            │                                           │
            │ ┌─────────────────────────────────────┐   │
            │ │ AsyncWebServer :80                  │   │
            │ │  ─ listens on ALL ifaces (sta +     │   │
            │ │    wg0 when up)                     │   │
            │ │  ─ same handlers serve LAN AND      │   │
            │ │    mothership-proxied requests      │   │
            │ └─────────────────────────────────────┘   │
            └───────────────────────────────────────────┘
```

---

## Library choices

### Device side

**`esp_wireguard`** by trombik
(<https://github.com/trombik/esp_wireguard>). Userspace WG over
LwIP, MIT-licensed, single-peer-focused. Properties:

- ~30 KB code.
- ~10 KB per-peer state (handshake context + send/recv buffers).
- API: `wireguardif_init` / `wireguardif_add_peer` / handshake
  status read / `wireguardif_shutdown`.
- Pure synchronous control surface, internal task drives keepalive.

PIO consumption (to be added in WG.2):
```
lib_deps = trombik/esp_wireguard @ ^0.4.0
```
(pin exact version once we've smoke-tested.)

### Server side

Kernel WireGuard module via `wireguard-tools` (`wg`, `wg-quick`,
`wg syncconf`). The mothership host already runs Linux with Flask;
adding `apt install wireguard-tools` and managing `/etc/wireguard/
wg0.conf` from Flask is straightforward.

**No** Python WG library — kernel does crypto, Flask just shells
out for peer add/remove + reads `wg show` for handshake status.

---

## Phased implementation plan

### Phase WG.1 — Server-side WG listener + peer admin (configurable)
**Goal:** Mothership host has `wg0` up with operator-chosen subnet,
Flask routes can add/remove peers, manual smoke-test works from a
laptop peer. All configuration via the existing Flask admin UI —
**no separate CLI tool**.

- New Flask admin page **"Tunnel network"** under
  `/mothership/wg/setup`. Form fields:
  - **Subnet size** — dropdown:
    `/24 (254 peers)`, `/22 (1022 peers)`, `/20 (4094 peers)`,
    `/16 (65534 peers)`. Default `/24`. Below the dropdown a live
    counter "X of N IPs allocated" once peers exist.
  - **Subnet base** — text field, default `10.99.0.0`. Mothership
    always takes `.1`.
  - **Listen port** — number field, default `51820`.
  - **Public endpoint** — text field, default = host's own LAN IP
    (today `192.168.10.202:51820`). Operator types here when the
    box gets port-forwarded externally or behind a DDNS record.
  - **Initialise / Re-initialise** button (the destructive one) —
    confirmation modal warns "rewriting the subnet revokes all
    existing peer IPs; devices will need a fresh `openTunnel`
    action to re-bind".
- Backend (`app/mothership/wg_admin.py`):
  - `wg_initial_setup(subnet, port, endpoint)` — generates mothership
    static keypair, persists under `/etc/wireguard/`, writes
    `wg0.conf`, runs `wg-quick up wg0`.
  - `wg_add_peer(pubkey) -> assigned_ip` — atomically allocate next
    free IP in subnet, append `[Peer]` block, `wg syncconf`.
  - `wg_remove_peer(pubkey)` — strip block, `wg syncconf`.
  - `wg_peer_handshake_age(pubkey) -> seconds` — parse `wg show`.
  - `wg_state()` → current subnet / port / endpoint / mothership
    pubkey / peer count. Used by the admin page to render status.
- Flask admin routes:
  - `GET /mothership/wg/setup` — render the setup form (pre-filled
    if already configured).
  - `POST /mothership/wg/setup` — apply new settings (gated by the
    confirmation modal on the front end).
  - `GET /mothership/wg/peers` — list peers + handshake ages.
  - `POST /mothership/wg/peers` (admin-only) — manual add (testing).
  - `DELETE /mothership/wg/peers/<pubkey>` — manual remove.

**Exit criteria:** Operator can sign in to mothership dashboard,
open Tunnel network, pick `/24` (or larger) from the dropdown,
click Initialise — wg0 comes up, `wg show wg0` shows the configured
subnet, a hand-crafted client config from a Linux laptop can
`ping 10.99.0.1`.

### Phase WG.2 — Device WireGuardModule + actionOpenTunnel/Close
**Goal:** Device can bring tunnel up/down on action, key material
persists across reboot.

- New module `modules/wireguard/`:
  - Priority 14, requires `cert-manager` (for SecretsVault-encrypted
    key persistence).
  - `IWireguardProvider` interface — `bool isUp()`, `String
    publicKey()`, `String assignedIp()`, `up(...)`, `down()`,
    `lastHandshakeAgeSec()`.
  - On `begin()`: load WG keypair from SecretsVault, or generate
    new Curve25519 pair if missing.
  - Tunnel only goes up on explicit `up(server_endpoint,
    server_pubkey, assigned_ip)` call — NOT on boot.
  - UI tab "Tunnel" — status (down / connecting / up + age),
    counters (TX/RX bytes), manual reconnect for debugging.
- `MothershipService::actionOpenTunnel(params)`:
  - Replaces existing stub.
  - Params: `{endpoint, server_pubkey, assigned_ip}` (from
    enrollment response, also redundantly sent each `openTunnel`
    so device doesn't need persistent storage of these).
  - Calls `_wg->up(...)`. Polls handshake state for up to
    HANDSHAKE_WAIT_S (15 s); on success pushes action_result 200
    `{tunnel_ip}`, on timeout pushes 504.
- `MothershipService::actionCloseTunnel(params)`:
  - Calls `_wg->down()`. Pushes 200 unconditionally.
- Burst cadence after `openTunnel` succeeds — set
  `cadence_override_until_s` to bump check-in to every 5 s for
  300 s, mirroring how mship-ui session cadence worked. This keeps
  any follow-up actions (closeTunnel after idle) snappy.

**Exit criteria:** Manually queue `openTunnel` via existing Flask
admin route, device tunnel goes up, `wg show` confirms handshake,
device serial logs "tunnel up". Queue `closeTunnel`, tunnel goes
down.

### Phase WG.3 — Enrollment co-installs WG peer
**Goal:** A fresh device that submits a bootstrap token gets BOTH
its X.509 cert AND a wg-peer entry on mothership; subsequent
`openTunnel` actions just reference the already-allocated IP.

- Extend `POST /api/v1/enroll` request:
  - Device adds `wg_pubkey` field alongside the CSR.
- Extend `/api/v1/enroll` response:
  - Adds `wg_server_pubkey`, `wg_endpoint`, `wg_assigned_ip` —
    server calls `wg_admin.wg_add_peer(device_pubkey)` and returns
    the triplet.
- `CertManagerModule::enroll(token)` stashes the WG triplet
  alongside the cert + key. `IWireguardProvider::up()` reads them
  back as defaults (so `openTunnel` action with empty params still
  works after a reboot — device knows where to dial).
- `CertManagerModule::revoke()` → server-side endpoint also
  removes the WG peer.

**Exit criteria:** Factory-reset device, enter bootstrap token in
UI, queue `openTunnel` from mothership dashboard — tunnel comes up
within one check-in cycle.

### Phase WG.4 — mship-ui proxy via tunnel (operator-facing)
**Goal:** Operator clicks "Open UI" on device panel → gets the
device's real React UI within seconds.

- Replace dead `device_ui.py` with new `mship_ui_tunnel.py`:
  - `POST /mothership/devices/<id>/tunnel/open` →
    `queue_action(id, openTunnel, ...)`, return 202 + reqId.
  - `GET /mothership/devices/<id>/tunnel/status` → returns
    PENDING / UP / FAILED based on action_result + cached handshake
    state.
  - `GET /mship-ui/<id>/<path>` — when tunnel up, proxy to
    `http://{assigned_ip}/<path>` via `requests`. Pass through
    method / body / query / headers (minus Host).
- Browser flow:
  - Operator clicks "Open UI" → JS calls `POST /tunnel/open`,
    receives reqId.
  - JS polls `/tunnel/status?reqId=…` every 500 ms.
  - On status=UP → JS navigates to `/mship-ui/<id>/`.
- Idle timeout — Flask tracks `last_browser_request_ts` per device.
  Background thread queues `closeTunnel` when `now -
  last_browser_request_ts > tunnel_idle_timeout_s` (default 300 s).

**Exit criteria:** Operator can browse full device UI through
mothership — menu + every tab's content + config saves.

### Phase WG.5 — Cleanup & docs
**Goal:** Zero references to Phase 7 on the WG branch.

- Confirm no `mship-bridge` / `ui.fetch.bulk` / `asset-upload`
  references survived branch creation (we branched pre-Phase-7,
  shouldn't have any).
- Update `mothership-roadmap.md` Phase 3 to point at this doc.
- README updates: operator quickstart — screenshot of the Tunnel
  network setup page, sign-in → click Initialise → first device
  enrollment.
- Add openTunnel/closeTunnel + WG fields to the docs/api-contract
  doc if one exists.

---

## PKI ↔ WireGuard binding

| Operation | Cert side | WG side |
|---|---|---|
| Enroll | New ECDSA-P256 keypair + cert | New Curve25519 keypair, peer added to wg0 (DOWN on device) |
| Renew cert | New cert, same X.509 key | unchanged |
| Rotate WG key | unchanged | New Curve25519 keypair, peer re-add at server, swap stored config |
| Revoke | Cert serial → CRL | Peer removed from `wg0` |
| `openTunnel` | unchanged | Device calls `wireguardif_init` + `add_peer`, sends handshake |
| `closeTunnel` | unchanged | Device calls `wireguardif_shutdown` |

One device identity, one operator-facing kill-switch — revoking the
cert also tears down the tunnel route.

---

## Heap budget (target, with tunnel UP during operator session)

| Subsystem | Bytes |
|---|---|
| WG state (1 peer + handshake context) | ~10 KB |
| WG send/recv buffers (MTU 1420 × 4) | ~6 KB |
| LwIP netif for wg0 | ~2 KB |
| Check-in mTLS context (transient, only during POST) | ~50 KB peak |
| AsyncWebServer (existing) | ~10 KB |
| Free remainder | **>50 KB** |

With tunnel DOWN (idle device in fleet): zero WG cost, only normal
check-in transient.

Versus the Phase 7 worst case (bridge + bulk + check-in =
~150 KB needed against ~127 KB free → OOM): this is a structural
fix, not a tuning fix.

---

## Resolved questions (operator confirmed 2026-05-19)

1. ✅ **Endpoint** — local for now (`192.168.10.202:51820`),
   port-forwarded externally later. Stored as a settings field, not
   a `#define` — operator can repoint without rebuild.
2. ✅ **Subnet** — operator-selectable via Flask admin UI dropdown
   (`/24` 254 peers / `/22` 1022 / `/20` 4094 / `/16` 65534).
   Default `/24`. Mothership always at `.1` of the subnet. **Not a
   CLI tool, not a hardcode** — operator picks during initial
   tunnel setup or re-initialises if fleet outgrows the size.
3. ✅ **Tunnel lifecycle** — on-demand. Mothership control plane
   continues over mTLS check-in (existing). Tunnel raised via
   `openTunnel` action, torn down via `closeTunnel` after operator
   session idle timeout (default 300 s, configurable).

## Still-open questions (small, can defer)

- **AllowedIPs from device's perspective** — recommend `10.99.0.1/32`
  so device's LAN-internet stays direct, only mothership reachable
  via tunnel. Confirm during WG.2.
- **MTU** — leave WG default 1420; revisit only if fragmentation
  observed.
- **Persistent keepalive** — 25 s default for NAT-traversal; might
  be unnecessary while tunnel is short-lived (operator sessions
  ~minutes), but cheap to leave on.
- **Tunnel idle timeout default** — 300 s feels right but defer
  binding until WG.4 when we see real operator usage.
- **Multi-operator concurrent sessions on same device** — if two
  operators open UI of the same device, second click is a no-op
  (tunnel already up, share the proxy). closeTunnel fires only
  when ALL operator sessions are idle. Trivial counter in Flask.

---

## What lands first

The first commit on `feature/wireguard-tunnel` (all three repos) is
**just this plan doc**, after operator review. Code starts in
Phase WG.1.
