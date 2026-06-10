# Device-mesh discovery + aggregated web + gateway reporting — strategy & plan

> **Scope.** Analysis + strategy deliverable. **No code in this task** — this is the
> roadmap to act on later. Findings below are verified against source (`file:line`).
> Three repos: `esp-rack` (framework + modules), `esp-rack-light-demo` (consumer),
> `esp-update-server-master` (Python mothership).

---

## 1. The vision (restated)

1. **Peers find each other** on the LAN over **mDNS**, gated by an **internal key**
   ("like authorization") so a device only accepts *our* devices.
2. **Our web auto-discovers** those peers, **pulls their manifests**, and builds a
   **consolidated panel** that lists the devices on the network.
3. Discovery must span **same-type** (same project) **and cross-project** devices —
   the only requirement being a **web/manifest version we support**.
4. Any device then acts as a **gateway to the external system**: it reports the
   **list of nodes in its sub-network** upward to the mothership server.

The governing compatibility key is therefore **not the project** but the **manifest
schema version** — a different project with a schema we understand is renderable.

---

## 2. Current state (verified)

### 2.1 Discovery substrate — mostly present, three gaps
- **Advertises** `_esprack._tcp` on port 80, hostname `<project>-<mac12>`, TXT keys
  `device_id` / `project` / `fw` — `MdnsService.cpp:235-241`.
- **Identity**: `DeviceIdentity::canonical()` = `<project>-<mac12>-<uid8>`, where
  `uid8` is the immutable eFuse `OPTIONAL_UNIQUE_ID` (anti-clone) —
  `DeviceIdentity.h:225-227, :149-187`.
- **Browse exists but is blocking + lossy**: `browseService()` calls
  `MDNS.queryService()` (~3 s block) and the on-demand `mdns.refreshServices`
  action spawns a 4 KB browse task — `MdnsService.cpp:112-145, :271-307`. **TXT
  records are discarded** ("future enhancement") — `:302-303`.
- **GAPS**: (a) no continuous/periodic browse with a peer cache, (b) no TXT
  extraction, (c) no **auth/trust tag** in the advert and no verification of peers.

### 2.2 Version axes — the compatibility key already exists
- `schemaVersion` (manifest envelope, currently **2**) — emitted by `ManifestBuilder`.
- `device.frameworkVersion` (`ESPRACK_VERSION_STR`, e.g. `0.2.5`) — `Version.h:21-28`.
- `device.version` (consumer app fw) — also the mDNS `fw` TXT.
- → **`schemaVersion` (+ a `frameworkVersion` floor) is the renderability gate.**
  It is **not** currently advertised over mDNS (only `fw` is).

### 2.3 Manifest pipeline — clean, but frontend is same-origin-locked
- Backend: `GET /rest/uiManifest` (`UI_MANIFEST_PATH`), **two-tier** (anon stub /
  authed full), LittleFS-cached (`.manifest.anon.json` / `.authed.json`), CRC32
  signature, ETag + 304 — `WebManager.cpp:408-493`, `WebManager.h:25,383-388`.
- Manifest shape: `features[]` → `menu` / `tabs[]` / `actions[]` / `component` /
  `route`; **REST/WS paths are ABSOLUTE** (`/rest/wifi/status`, `/ws/wifi/status`)
  — `manifest.ts`, `DynamicFeature.tsx`.
- Frontend fetch: `AXIOS` with `baseURL:'/rest/'` (**relative → same-origin**),
  `WEB_SOCKET_ROOT` from `window.location` (**same-origin**), `toAxiosPath()`
  strips `/rest/` — `api/endpoints.ts:6-48`, `DynamicFeature.tsx:20-21`.
- Auth injected by an AXIOS `transformRequest`: `Authorization: Bearer <jwt>` from
  `localStorage` (+ `X-Client-Key`, `X-Current-Page`) — `api/endpoints.ts:15-44`.
- **HARD CONSTRAINT**: there is **no way to point the frontend at another device's
  base URL** — `baseURL` is baked in at module load. Browser-direct cross-device
  aggregation = a real refactor (per-device AXIOS factory + per-device WS root +
  device-selection context + ManifestLoader redesign + cross-device auth).

### 2.4 Trust primitives — one shared anchor, no LAN peer-auth
- **JWT secret is per-device** (random on first boot, `securitySettings.json`) —
  **not** shareable across a fleet without re-flash. HMAC-SHA256, claims
  `{username, admin}` — `SecuritySettingsService.*`, `ArduinoJsonJWT.cpp`.
- `FACTORY_CERT_BOOTSTRAP_TOKEN` is **single-use**, wiped on first enroll — not a
  reusable peer key.
- **The one fleet-wide trust anchor: the mothership CA root** (`ca_bundle_pem`) —
  every enrolled device trusts the **same CA**, loaded into `TLSContextService`.
  Device A *could* verify device B's cert via this shared root — but **no device
  runs an HTTPS server with peer-cert validation today**, and the web is plain
  HTTP:80. `CertManagerService.*`, `TLSContextService.h:35-44`.
- WebManager enforces `WebAuthLevel` (Public / Authenticated / Admin) via
  `securityManager->wrapRequest()` (JWT from header or `?access_token=`).
- **GAP**: no "trusted peer" auth path, no fleet/project PSK.

### 2.5 Mothership path — extensible, and `rest.proxy` is a gift
- Check-in `POST /api/v1/checkin` over a **persistent mTLS client**, body from a
  BSS `StaticJsonDocument<3072> _reqDoc`: `deviceId, fwVer, hwVer, hwRev,
  uptimeSec, freeHeap, maxAlloc, manifest_rev, action_results[], wg{}` —
  `MothershipService.cpp:585-694`.
- Response `{actions[], nextCheckInSec}`; action types incl. **`rest.proxy`**
  (server calls any local `/rest/*` on the device) and `config.dump` (chunked) —
  `:818-948, :1094-1235`.
- Server: `device_api.py:_handle_checkin()` + `state.py` registries (`DEVICES`
  volatile, `TRUSTED_DEVICES`/`TRIAL_DEVICES` persistent, `COMMAND_QUEUE`,
  **`MANIFEST_CACHE`** — it already caches device manifests via `rest.proxy`).
- **GAP**: **no** `gateway` / `node` / `subnet` / `topology` concept server-side.
  Natural slot: `lan_nodes[]` in check-in → `DEVICES[id]["lan_nodes"]`.

---

## 3. The pivotal decision — aggregation architecture

Because the frontend is same-origin-locked (§2.3), **how** the consolidated web
reaches peer devices is the decision that drives 60% of the effort. Three options:

| | **D — Directory of links** | **Y — Gateway proxy (recommended)** | **X — Browser-direct** |
|---|---|---|---|
| Browser talks to | only host device | only host device (same-origin) | every device (cross-origin) |
| Peer features rendered in-panel | ✗ (link out to peer's own UI) | ✓ (host relays REST) | ✓ (per-device AXIOS) |
| Cross-device auth | n/a | host↔peer only (PSK/mTLS) | browser↔every-peer (CORS+JWT) |
| Frontend change | small (a Devices page) | medium (device dimension + proxied base) | large (per-device AXIOS/WS factory, device context, ManifestLoader redesign) |
| Live WS | peer's own UI | host must relay (hard) → poll v1 | direct per-device (easy) |
| Host load | none | relays all peer traffic (bottleneck at scale) | none |
| Security surface | minimal | host is the only attack point | every device must accept cross-origin operator |

**Recommendation: ship D first, evolve into Y.**
- **D** delivers the user's literal "list of devices" immediately, with near-zero
  risk, and reuses each device's *own* same-origin UI for deep interaction.
- **Y** adds true in-panel consolidation later **without** breaking the same-origin
  frontend assumption — the host device becomes an **aggregating gateway**, which
  is *the same device* that reports to the mothership (§1.4). One concept, two uses.
- **X** is only worth it if the host-as-bottleneck or WS-relay cost is unacceptable
  *and* we accept the cross-origin/CORS/multi-login complexity. Defer / probably skip.

---

## 4. The trust model — a project PSK (the "internal key")

The user's "internal key … like authorization" maps cleanly to a **project-scoped
pre-shared key (PSK)** — one secret shared by all devices of a deployment. Two uses:

1. **mDNS auth tag (anti-spoof filter).** Add TXT `sig = HMAC-SHA256(PSK,
   device_id | epoch_window)` (rolling, ~5-min window). A peer recomputes and
   accepts only matching adverts → "this is one of ours," replay-bounded. Cheap,
   reuses existing HMAC (`ArduinoJsonJWT.cpp`).
2. **Peer-API auth (host↔peer).** A new `WebAuthLevel::Peer` (or a header
   `X-Peer-Auth`) accepted when the caller proves PSK knowledge via
   challenge-response (device issues nonce → caller returns `HMAC(PSK, nonce)`),
   or a short-lived PSK-derived bearer. Lets the gateway pull a peer's full
   (authed-tier) manifest without an operator login on every peer.

**Where the PSK comes from** (pick one — see open questions):
- (a) operator-entered shared passphrase (simplest, manual);
- (b) **mothership-distributed at enroll** (a `fleet_psk` field in the enroll
   response, stored beside the cert — fits the existing PKI trust chain, rotatable
   centrally) — *recommended*;
- (c) derive from the shared CA (HKDF over a CA-bound value) — zero new
   distribution, but couples PSK rotation to cert rotation.

**High-assurance alternative:** skip the PSK and use **mTLS with the shared
mothership CA** for the peer-API (each device runs a tiny HTTPS peer endpoint;
verifies caller cert against `ca_bundle_pem`). Strongest, but adds an HTTPS server
per device + heap cost on no-PSRAM C3 (§ memory note). Recommend PSK for v1,
keep mTLS-CA as the hardening path.

---

## 5. Phased plan

Ordering = value-early × low-risk-first. Each phase is independently shippable.

### P0 — Discovery substrate (firmware, `esp-rack`)
**Goal:** every device continuously knows its trusted, compatible peers.
1. **Auth-tagged advertise** — extend `MdnsService.cpp:235-241` TXT with
   `schema` (manifest `schemaVersion`), `fw_fx` (`frameworkVersion`), and
   `sig` (rolling PSK HMAC, §4.1). Keep `device_id`/`project`/`fw`.
2. **Continuous browse + TXT extraction** — periodic background browse of
   `_esprack._tcp` (reuse the existing task pattern at `:112-145`, but make the
   stack **static/BSS** per the heap rule), and **pull TXT** (close the
   `:302-303` gap). Maintain a **bounded BSS peer cache** (e.g. ≤32 peers) with
   `{id, project, ip, port, fw, schema, trusted(sig ok), compatible(schema in
   range), lastSeen}` and a TTL/eviction.
3. **`/rest/peers` endpoint** — serve the verified peer list to the browser
   (browsers can't do mDNS). Authed tier. This is the seam the web reads.

**Acceptance:** on a 3-device LAN each device's `/rest/peers` lists the other two
as `trusted:true, compatible:true`; an mDNS advert without a valid `sig` shows
`trusted:false`. No heap-task holes (static stack), peer cache is BSS.

### P1 — Devices dashboard (frontend, **Option D**)
**Goal:** the user's "consolidated panel with a list of devices," shipped.
1. A **`DevicesService`** (firmware) registers a top-level `devices` feature
   (manifest), so it appears in the tree with its own icon.
2. Frontend **Devices page** — fetch `/rest/peers`, render a table/cards:
   name · project · fw · schema · health(reachable) · **compatible?**. Each row
   links to `http://<peerIP>/` (the peer's own same-origin UI) and/or a
   "summary" drawer.
3. **Manifest summary pull** — for each peer, fetch a *lightweight* summary
   (feature count, version) via the host's proxy (P2 seam) or a peer
   `/rest/peers/self/summary`. Show "what this device offers" without full render.
4. **Self-vs-peer & grouping** — group by project ("same-type") vs "other
   projects (compatible)"; flag incompatible schema explicitly.

**Acceptance:** operator opens **Devices**, sees all trusted peers grouped by
project with health + compatibility, and can jump to any peer's UI in one click.

### P2 — Gateway proxy + in-panel consolidation (firmware + frontend, **Option Y**)
**Goal:** render a peer's features *inside* the host panel ("consolidated web").
1. **Peer proxy** (firmware) — `(/rest|/ws)/peer/<id>/<path>` on the host: resolve
   `<id>`→ip from the peer cache, authenticate host→peer with the PSK (§4.2),
   relay the request, stream the response. Mirror of the existing server→device
   `rest.proxy` (`MothershipService.cpp:1094`), but **device→peer**, with a
   `max_alloc` guard like the manifest 503.
2. **Frontend device dimension** — extend the manifest/menu model with a
   `device` axis: render each peer's manifest features under a device group,
   targeting the proxied same-origin base (`/rest/peer/<id>/…`). Reuse
   `DynamicFeature` unchanged — only the path prefix differs (still same-origin,
   so **no AXIOS refactor**).
3. **Live data** — v1 **poll** proxied REST for peer "live" tabs (WS-relay is
   hard); revisit WS-relay or browser-direct-WS only if polling is insufficient.

**Acceptance:** operator expands a peer in the host's tree and edits/reads its
real settings through the host, no second login, host stays heap-stable under a
peer fan-out load test on C3.

### P3 — Gateway → mothership node reporting (firmware + server)
**Goal:** any gateway reports its sub-network upward to the external system.
1. **Check-in `lan_nodes[]`** — when a device is in *gateway role*, append its
   verified peer list to the check-in body (`MothershipService.cpp:585-694`),
   **chunked** across cycles if large (reuse the `action_results` chunking
   discipline). Fields per node: `{device_id, ip, project, fw, schema, trusted,
   lastSeen}`.
2. **`scanSubnet` action** — server-initiated on-demand refresh: new action type
   in the device dispatch (`:818-948`) + handler (spawn browse, push result via
   `action_results`), and the server-side queue/dispatch in `device_api.py`.
3. **Server registry + role** — `DEVICES[id]["lan_nodes"]` + a `gateway_role`
   flag in `state.py`; emit a `subnet_scan` device-event. Optionally: have the
   server fetch peer manifests **through the gateway** (extend the existing
   `MANIFEST_CACHE` + `rest.proxy` to `rest.proxy` *via* the gateway proxy of P2)
   → server-side fleet topology with per-node schemas.
4. **Server UI** — a topology/subnet view grouping nodes under their gateway.

**Acceptance:** mothership dashboard shows each gateway with the live list of
nodes in its subnet; `scanSubnet` refreshes within one check-in round-trip.

### P4 — Hardening & ops
- PSK **rotation** (central, via mothership enroll/check-in action).
- mTLS-CA peer-auth as an optional high-assurance mode (§4 alternative).
- Compatibility **range policy** (which `schemaVersion`s the host will render;
  graceful "view-only / open native UI" for out-of-range peers).
- Soak: peer cache churn, proxy heap deltas on C3 (must obey the
  fragmentation gates — see `memory-fragmentation-master-plan.md`).

---

## 6. Cross-cutting: memory discipline (C3/C6, no PSRAM)

This feature adds periodic network work + buffering — must obey the existing
anti-fragmentation rules (`docs/plans/memory-fragmentation-master-plan.md`):
- **Peer cache in BSS**, bounded + evicting (no unbounded `std::vector<String>`
  growth; prefer fixed `char[]` / `std::string` with `reserve`).
- **Browse task stack static/BSS** (the current 4 KB browse task is heap —
  `MdnsService.cpp:120` — same class of hole as the A5 list in the master plan).
- **Proxy guarded**: check `getMaxAllocHeap()` before relaying; 503/"retry" under
  pressure, never OOM-panic (mirror the manifest guard `WebManager.cpp:477-493`).
- **No per-cycle churn**: reuse one buffer for browse/proxy like `_reqDoc`.

---

## 7. Open questions (need your call)

1. **Aggregation depth for v1** — ship **D (directory + links)** first and add
   **Y (in-panel proxy)** in P2 (recommended), or go straight to in-panel?
2. **PSK source** — operator passphrase, **mothership-distributed at enroll**
   (recommended), or CA-derived? And is a lightweight rolling-HMAC mDNS tag +
   PSK peer-auth acceptable, or do you require **mTLS-CA** peer-auth from day one?
3. **"Web version we support"** — confirm the gate is **`schemaVersion`** (+ a
   `frameworkVersion` floor). What's the supported range policy for incompatible
   peers — hide, or show as "open native UI only"?
4. **Gateway election** — is *every* device a potential gateway (any one reports),
   or is the gateway **designated** (by the mothership / by config)? Affects P3
   `lan_nodes` emission gating.
5. **Live data across devices** — is **polling** acceptable for peer "live" tabs
   in v1, or is real-time WS across devices a hard requirement (→ pushes toward
   Option X for those tabs)?

---

## 8. TL;DR

The substrate is 70% there: devices already advertise identity+version over mDNS,
the manifest is clean and cacheable, the mothership check-in + `rest.proxy` are
ready to carry topology, and the shared mothership CA is a latent fleet-trust
anchor. The real work is four seams: **(P0)** auth-tagged continuous mDNS browse +
`/rest/peers`; **(P1)** a Devices dashboard (directory); **(P2)** a device→peer
**gateway proxy** so the same-origin frontend renders peers in-panel without an
AXIOS refactor; **(P3)** `lan_nodes` check-in + a `scanSubnet` action + server
topology. Trust is a **project PSK** (rolling mDNS HMAC + peer-API auth), with
**mothership-distributed PSK** and **mTLS-CA** as the hardening path. The
**aggregating device is the gateway** — one concept serves both the consolidated
web and the upward node reporting.
