# API Standardization — survey, options, target design, migration plan

> Status: PROPOSAL v2 (2026-06-12). Produced from a 4-track survey: full REST
> endpoint inventory, SPA routing/tab mechanics audit, auth surface audit,
> and field research on embedded API standards (Tasmota / ESPHome / Shelly
> Gen2 / Home Assistant / EMS-ESP32 / ESP-IDF restful example).
> Raw survey reports: session artifacts 2026-06-12 (workflow
> `api-standardization-survey`).
>
> v2 refinements (owner decisions):
> 1. **No legacy, no aliases** — the product has not shipped yet, so the
>    rename is a clean cut: `/rest/*` disappears entirely in the same
>    generation that introduces `/api/v1/*`. No dual-matching, no M6.
> 2. **Machine access covers EVERYTHING on the device, read AND write** —
>    via the same unified API, role-gated. Every feature exposes a flat
>    machine-friendly `state` resource (GET to read values, POST to set
>    them) alongside the UI-only `form` resource (Form-DSL envelope).

---

## 1. Where we are (verified)

### 1.1 Endpoint zoo — ~50 HTTP endpoints, five naming styles at once

| Style | Examples | Count |
|---|---|---|
| `<thing>Form` suffix | `/rest/wifiForm`, `/rest/apForm`, `/rest/ntpForm`, `/rest/telegramForm` | ~6 |
| bare resource | `/rest/mothership`, `/rest/wireguard`, `/rest/certManager`, `/rest/peers` | ~5 |
| namespaced | `/rest/system/status`, `/rest/system/mdns`, `/rest/fs/list`, `/rest/ota/settings` | ~15 |
| legacy flat | `/rest/systemStatus` (coexists with `/rest/system/status`!), `/rest/autoUpdateSettings` | ~4 |
| action verbs | `/rest/restart`, `/rest/wifiScan`, `/rest/action/mqtt.attachTopic` (auto-derived) | ~12 |

- No versioning anywhere.
- Error bodies are a zoo: bare `send(400)`, `{"error":"heap_pressure","retryAfter":1}`,
  `{"ok":false,"message":"…"}`, `{"ok":false,"err":"…"}`, literal `"null"` (now fixed).
- Two payload dialects: Form-DSL envelope (UI forms) vs raw JSON
  (`/rest/peers`, `/rest/systemStatus`, fs endpoints).
- `rest.proxy` (mothership in-process dispatch via `IWebFeatureEntry::proxyDispatch`)
  must keep working for any rename.

### 1.2 Tab deep-linking — the router ALREADY does it

Verified end-to-end (`useRouterTab.ts`, `DynamicFeature.tsx:238-254`,
`AuthenticatedRouting.tsx:84-91`): the active tab **is** a URL path segment.
`/lightState/settings`, `/system/status` (= Resources tab) are real, restorable
URLs; menu `expandTabs` leaves emit exactly these links. There is **no
`?tab=` state to add — the mechanism exists.**

Why it *feels* broken:
1. **No canonical-URL affordance** — nothing in the UI exposes "this tab's
   link" and the docs never state the `/feature/<tabKey>` grammar.
2. **Aggregator features** (`system`) get tabs injected by many modules
   (`addTabToFeature`); a tab key is only addressable once its module
   contributed it — and tab KEYS are ad-hoc (`status` = "Resources").
3. **Cold-load**: routes are built from the manifest; until the manifest
   arrives the catch-all can bounce a deep link (must verify & fix the
   loading gate so unknown-yet routes show the loader, never redirect).
4. REST paths (`/rest/system/status`) and SPA paths (`/system/status`)
   collide visually — operators paste the wrong one.

### 1.3 Auth surface — fine for a LAN UI, not for a machine API

Verified (`ArduinoJsonJWT.cpp`, `SecuritySettingsService.cpp`):
- Bespoke JWT HS256; payload `{username, admin}` — **no `exp`/`iat`**, tokens
  live until the secret rotates.
- Roles: binary admin/guest; `Authenticated` vs `Admin` predicates; failures
  always 401 (никогда 403).
- WS auth: `?access_token=` only by frontend convention — **WsManager performs
  no upgrade-time auth gate**.
- `/rest/securitySettings` is `Authenticated`, not `Admin` (admin check lives
  in the UI only).
- mTLS device↔mothership channel exists and is the right trust anchor for
  the fleet; `rest.proxy` deliberately bypasses JWT (documented).
- No API keys / scopes / rate limits; CORS builder hook is a dead stub.

---

## 2. Options considered

### Option A — Pragmatic versioned REST (`/api/v1`) + conventions ← **RECOMMENDED**
Plain JSON resources, URI versioning, RFC 9457 errors, two-token bearer auth,
OpenAPI generated **off-device at build time** from our own registry.
- Matches what the ESP-IDF official restful example, EMS-ESP32 and Home
  Assistant converged on.
- Near-zero RAM cost; mostly renames + one error helper + JWT claims.
- Keeps the existing manifest/registry as the single source of truth.

### Option B — JSON-RPC 2.0 single endpoint (Shelly Gen2 style) — rejected
`POST /rpc {method:"Wifi.GetConfig"}`. Compact and uniform, but: breaks REST
semantics the mothership already uses, maps poorly to OpenAPI, makes browser
caching/ETags useless, and would force a rewrite of every existing consumer
in one step (no alias-based gradual migration).

### Option C — JSON:API / HAL hypermedia — rejected
+100–200 B envelope per resource on a no-PSRAM C3, mandatory `data/type/id`
wrappers, no realistic embedded client benefit. Hypermedia relations belong
in the OpenAPI spec, not in every 2 KB response.

---

## 3. Target design (Option A, concrete)

### 3.1 URL grammar — the form/state split

Every feature exposes up to TWO faces of the same StatefulService:

```
/api/v1/<feature>/state     MACHINE face — flat JSON values
/api/v1/<feature>/form      UI face — Form-DSL envelope (labels/icons/"o" strings)
─────────────────────────────────────────────────────────────────
GET  /api/v1/                            capability/root object
GET  /api/v1/system/status               flat system health (machine)
GET  /api/v1/system/identity
GET|POST /api/v1/wifi/state              read/set wifi values (machine)
GET      /api/v1/wifi/form               wifi Form-DSL (web UI only)
GET|POST /api/v1/light/state             read/set light values
GET      /api/v1/light/form
GET  /api/v1/mesh/peers                  raw JSON (single-face resources keep one path)
GET  /api/v1/fs/list?path=…
POST /api/v1/actions/<id>                ALL verbs: restart, factory-reset,
                                         wifi.scan, mqtt.attach-topic, …
POST /api/v1/auth/token                  (≡ old /rest/signIn)
GET  /api/v1/auth/verify
```

Rules:
- **`state` is the machine contract**: GET returns flat key/value JSON
  (today's POST input shape — `update()` already consumes exactly this),
  POST sets any subset of values. This is how an external server reads AND
  writes *everything the device has*, uniformly, without parsing UI
  envelopes. Implementation reuses the readers that already exist: the
  plain/WS reader (`staRead`-style) backs GET, the existing `update()`
  backs POST — no per-module rewrite, just registration plumbing in
  WebFeatureEntry (one extra route per feature).
- **`form` is UI-only**: the Form-DSL envelope, GET-only (the UI saves via
  `state` POST — same updater as today, so nothing changes functionally).
- **Actions are uniform**: `POST /api/v1/actions/<dotted.id>` from the
  existing action registry; hand-rolled exceptions (`/rest/restart`) die.
- Single-face resources (peers, fs, manifest) keep one canonical path.
- The `…Form` suffix and the entire `/rest` prefix disappear — no aliases
  (pre-release, nothing to stay compatible with).

### 3.2 Data API (accumulated/collected data the server pulls)

For data the device ACCUMULATES over time (sensor series, event logs) —
distinct from current-state values, which live under `<feature>/state`:

```
GET /api/v1/data                                  list of registered series
GET /api/v1/data/<series>?limit=50&cursor=<op>    paged, never unbounded
GET /api/v1/data/<series>/latest
```
- Pagination is mandatory (no-PSRAM C3: responses target ≤4 KB; the adaptive
  grow/chunk layer in `HttpEndpoint.h` is the safety net, not the plan).
- `series` registry follows the module pattern (a module registers its
  series the way it registers features today).
- Write path for data is NOT needed (the device is the producer); commands
  that influence collection are normal `state` fields or `actions`.

### 3.3 Errors — RFC 9457 `application/problem+json`

One helper (`sendProblem(req, status, title, detail, retryAfterSec=-1)`),
~150 B/response:
```json
{ "status": 503, "title": "heap_pressure", "detail": "…", "retryAfter": 2 }
```
- All new `/api/v1` handlers use it; the axios layer already understands
  `retryAfter` (503 auto-retry, shipped 2026-06-12).
- 401 = no/invalid token, **403 = valid token, insufficient role** (new).

### 3.4 Auth — three credentials, one header

| Credential | Issuer | Lifetime | Claims | Use |
|---|---|---|---|---|
| Human JWT | device (`/api/v1/auth/token`) | `exp` = 24 h (NEW) | `username, role, exp, iat` | web UI |
| Machine token | device admin UI ("create access token" page, HA-style) | long-lived (revocable list) | `role` chosen at creation, `exp` 10 y | external server (read AND write) |
| mTLS client cert | cert-manager/mothership CA | rotation per PKI | CN = device id | checkin + rest.proxy (unchanged) |

Roles (the `role` claim) gate the unified API like this:

| Role | `state`/`data`/`form` GET | `state` POST + `actions` | security/PKI/fs/firmware |
|---|---|---|---|
| `reader` | yes | no (403) | no |
| `operator` | yes | yes — full read/write of device values | no |
| `admin` | yes | yes | yes |

So "the server works with everything on the device — reads and sets
values" = an `operator` machine token. A monitoring-only consumer gets
`reader`. Admin-only surface (user management, JWT secret, cert rotation,
factory reset, firmware upload) stays human/admin.

- Same bespoke HS256 signer — just add `exp`/`iat`/`role` claims + clock
  check (NTP present; allow ±skew, reject `exp` only when time is synced).
- **403** on role mismatch (today everything is 401).
- Machine tokens: per-token random id + role + revoked flag persisted in
  `securitySettings.json` (bounded list ≤8), token value displayed once at
  creation.
- Close the audited holes while at it: WS upgrade-time auth gate in
  `WsManager::addEndpoint` (check `?access_token=` predicate before accept);
  security settings POST → Admin server-side.

### 3.5 OpenAPI — generated at build time, never served by the device

- The WebManager registry is already a machine-readable inventory
  (`/rest/uiManifest` + `/rest/system/endpoints`). A Python script
  (`scripts/gen_openapi.py`, runs in CI/pre-commit) renders
  `api/openapi.yaml` from a registry dump + a small per-endpoint
  schema-hints file; `spectral lint` gates it.
- The mothership gets a generated typed client (`openapi-generator-cli`,
  python/pydantic) — the server-side consumer the user asked for.
- Spec hosted in repo / on mothership; device `servers:` entry is filled
  with the device IP by the consumer.

### 3.6 Tab deep-links — formalize what exists

- Document the grammar `/feature/<tabKey>` as API: tab keys become part of
  the public contract (stable, kebab-case; rename `status`→`resources`
  where the label lies).
- Fix the cold-load gate: never `<Navigate>` away while the manifest is
  still loading; show the loader on unknown paths until manifest resolves,
  THEN match (one guard in `AuthenticatedRouting`).
- Optional nicety: a "copy link to tab" affordance in the tab bar.

---

## 4. Migration plan (CLEAN CUT — pre-release, no legacy)

| Phase | Scope | Risk |
|---|---|---|
| C0 | `sendProblem()` helper (RFC 9457 everywhere) + 401/403 split + JWT `exp/iat/role` claims (web UI re-logins once) | low |
| C1 | THE rename, one generation: registry mounts only `/api/v1/...`; `state`/`form` split wired in WebFeatureEntry (GET form, GET/POST state); all actions under `/api/v1/actions/<id>`; `/rest` ceases to exist; `rest.proxy` matches the new paths only; `uiManifest` schemaVersion → 3 | medium (wide but mechanical; no compat code at all) |
| C2 | Frontend in the SAME commit/firmware: `API_BASE_URL='/api/v1/'`, `toAxiosPath` for the new prefix, manifest consumers, WS unchanged (`/ws/*` stays) | rides with C1 |
| C3 | Machine tokens page + `reader`/`operator`/`admin` role gates + WS upgrade auth gate + security POST→Admin | medium (auth — test in isolation) |
| C4 | `scripts/gen_openapi.py` from the registry dump + spectral in CI + generated typed Python client for the external server | low, off-device |
| C5 | Data API (`/api/v1/data/<series>`) module pattern + first real series | new feature |

Notes:
- C1+C2 ship together as ONE firmware (the UI and API are one bundle via
  PROGMEM_WWW — there is no skew window by construction).
- Mothership `rest.proxy` call sites (server-side scenarios) are updated to
  the new paths in the same change set — pre-release, both ends are ours.
- mesh schemaVersion=3 means a (hypothetical) older bench device simply
  shows the new ones as "incompatible web" in Discovery — acceptable on a
  bench, irrelevant after the cut.

---

## 5. Explicitly rejected from the research

- **Hash-router (#/) for the SPA** — recommended by generic embedded guides,
  rejected here: history-API routing + manifest catch-all already works and
  gives real URLs; switching would regress UX for zero gain.
- **Serving openapi.json from the device** — flash/OTA fragility for nothing.
- **PASETO** — no viable ESP32 library (verified 2026-06).
- **RS256/ES256 tokens** — ~20 KB+ verify cost; HS256 + HW SHA-256 stays.
- **JSON:API/HAL envelopes** — payload tax on a 9-19 KB max_alloc target.
