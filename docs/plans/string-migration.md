# Arduino String → std::string / fixed-buffer migration

**Status:** plan only — no code edits yet.

## Why

Arduino `String` uses doubling-realloc growth. Under the C3's tiny
heap budget (320 KB SRAM total, post-boot max_alloc dips to ~17 KB,
worst-case `min_free_ever` measured at **2 KB**) every `+=` or
`String(int)` ctor can trigger a realloc that fragments the heap.
Symptoms already observed in this firmware:

* `/rest/uiManifest` payload truncates mid-build (AsyncResponseStream
  uses `StreamString _content` internally) → SPA renders only the
  legacy "Security" tab fetched via the tiny `/rest/features`
  endpoint. *Fixed in this branch by caching the manifest in
  `std::string` and serving via `AwsResponseFiller`.*
* TLS handshake -32512 OOM crashes after ~7–15 min of uptime —
  PEM blobs held as String members reallocated on every cert swap.
* Long uptime soak: `min_free_ever` slope ~ –19 KB/min — Strings
  in MQTT topic vectors + WS payload buffers churn permanently.

## Scope

In-scope: `esp-rack/lib/ESPRack/`, `esp-rack/modules/`,
`esp-rack-light-demo/src/`.

Out-of-scope: `.pio/libdeps/` (third-party libs we don't control —
AsyncWebServer, ArduinoJson, espMqttClient, HTTPClient, etc.).
At those seams we keep `String` but minimize its lifetime: build
the payload in `std::string`, construct a temporary
`String(c_str(), size())` only at the API call site.

## Migration buckets (audit findings)

* **~115 MEMBER fields** — persist on heap for object lifetime.
  Highest ROI to migrate. PEM blobs (TLSContext, CertManager) and
  MQTT topic vectors are the worst offenders.
* **~52 RETURN-by-value functions** — allocate per call. Hot ones:
  `DeviceIdentity::*`, `MothershipService::activeBaseUrl/...`,
  `IWireguardProvider::publicKey/assignedIp`, `SecurityHash::*`,
  `SecretsVault::encrypt/decrypt`.
* **~210 PARAM uses** — mostly `const String&` (cheap, just a
  reference), some by-value (wasteful). Low priority unless the
  caller is on a hot path.
* **~430 LOCAL uses** — short-lived, low fragmentation pressure.
  `FormBuilder.h` alone has ~40 in factory methods called once
  at boot. Skip unless they show up in churn traces.
* **~200 BOUNDARY uses** — required by third-party API. Keep, but
  surround with `std::string` internally.

## Replacement table

| Today | Replace with | When |
|---|---|---|
| `String _foo;` (variable size, bounded) | `std::string _foo;` + `reserve()` in ctor | growing/shrinking content |
| `String _key;` (fixed-size, ≤64 B) | `char _key[N];` + `strlcpy` | identifiers, keys, IPs |
| `std::vector<String>` | `std::vector<std::string>` | container of dynamic strings |
| `String foo()` returning a member | `const char* foo()` (point at member's `c_str()`) or `const std::string& foo() const` | identity/getter |
| `String foo()` returning computed value | `void foo(char* out, size_t n)` or `std::string foo()` | minimize per-call cost |
| `const String& s` parameter | `const std::string& s` or `const char* s` | dependent on caller mix |
| `request->send(200, "...", str)` | build into `std::string`; pass `String(s.c_str(), s.size())` at call site | boundary with AsyncWebServer |

## Phase ordering (low-risk leaves → high-blast headers)

### Phase 0 — preflight ✅ (done in this branch)
* WebManager manifest cache uses `std::string` + `AwsResponseFiller`
  (no Arduino String on the response path). Validates the std::string +
  ArduinoJson serialization pattern.

### Phase 1 — isolated fixed-size leaves
Goal: prove the `char[N]` pattern on members with hard size ceilings.
Zero public API change — these are internal-only.
* `modules/wireguard/include/WireguardService.h:48-61` — `priv_key`,
  `pub_key`, `server_pub_key`, `endpoint`, `assigned_ip` → `char[48]`,
  `char[48]`, `char[48]`, `char[48]`, `char[24]`.
* `lib/ESPRack/include/PersistentTlsClient.h:113,115` — `_current_host`
  (already mostly migrated to `_host_static[64]`), `_current_path` →
  `char[128]`.
* `modules/wifi/include/WiFiSettingsService.h:54-151` — `ssid[33]`,
  `password[65]`, `hostname[33]`. SSID is bounded by 802.11.

### Phase 2 — internal `std::string` members (no API impact)
Goal: stop heap fragmentation on the worst churners.
* `lib/ESPRack/include/TLSContextService.h:55-61` — 7 PEM Strings →
  `std::string` with `reserve(2048)`. PEM blob lifetime guaranteed
  by attachToClient contract; NetworkClientSecure stores raw `const
  char*` → `c_str()` is stable.
* `modules/cert-manager/include/CertManagerService.h:76-364` — 11 PEM
  + token Strings → `std::string`.
* `lib/ESPRack/include/WsManager.h:44-654` — event payloads → carry
  `std::string` instead of `String`. Per-frame churn drops to zero
  fragmentation.
* `modules/mqtt/include/MqttSettingsService.h:67-580` — internal
  vectors/sets of String → vectors/sets of `std::string`. Adapter
  function `attachedTopics()` keeps existing `const std::vector<String>&`
  return type until Phase 5.
* `modules/presence/include/PresenceService.h:31-116` — ClientPresence
  fields → `std::string`.

### Phase 3 — return-by-value hotspots
Switch hottest return-String functions to `const char*` (point at a
stable member) or `const std::string&`. Each touched function is
called from <5 call sites in framework + modules; track carefully.
* `IMothershipProfileProvider::activeName / activeBaseUrl / enrollUrl
  / checkinUrl / recoverUrl` — return `const char*` via cached
  `std::string` member.
* `IWireguardProvider::publicKey / assignedIp` — return `const char*`
  via member char[].
* `DeviceIdentity::*` — already cache in `static String` — convert
  to `static std::string` and return `const char*`.

### Phase 4 — secrets & crypto
* `SecurityHash::toHex/makeSalt/hashPassword` — accept output buffer
  `(char* out, size_t n)` instead of returning String.
* `SecretsVault::hexEncode/encrypt/decrypt` — same.
* `ArduinoJsonJWT::sign/encode/decode/getSecret/buildJWT` — same.

### Phase 5 — public interfaces (blast radius: ALL modules)
Save for last; do as one batch + version bump on framework.
* All `lib/ESPRack/include/I*Provider.h` headers that expose String.
* `lib/ESPRack/include/SecurityManager.h:20-28` (User struct).
* `lib/ESPRack/include/ConfigManager.h:101,593-597,681`.
* `lib/ESPRack/include/FormBuilder.h` (one-shot at boot, lower urgency
  but ~72 String mentions).
* `lib/ESPRack/include/Builder.h:95-98`.

### Phase 6 — demo + consumer apps
* `esp-rack-light-demo/src/LightStateService.h:49-173` — migrate
  alongside MQTT module API.
* MQTT topic/payload callbacks: stop wrapping `const char*` payload
  into `String` (no-op boundary that adds churn).

## Acceptance criteria (per phase)

1. Build clean: `pio run -e esp32-c3-devkitm-1` succeeds with zero new warnings.
2. RAM usage in linker report does NOT grow more than +1 KB.
3. Live soak test ≥ 30 min: `min_free_ever` improves vs baseline
   (today's worst: 2 KB; target: stay ≥ 16 KB).
4. Manifest fetch + first checkin + WG up/down cycle: HTTP=200 on
   all observed requests.
5. New numbers logged in `docs/plans/string-migration-results.md`
   per phase (heap free / max_alloc / min_free_ever before & after).

## Open questions

1. ESP-IDF arduino-esp32 String is from WString.h; ArduinoJson v6
   has first-class `std::string` support, but FormBuilder uses String
   pervasively. Verify ArduinoJson serializes correctly into
   std::string in our build — Phase 0 confirmed it works for the
   manifest case; assume yes for all uses.
2. `request->arg(const String&)` and header iteration return `String`
   by reference to internal storage. Adapter: copy into a local
   `std::string` once at handler entry, never mention String again.
3. Telegram / MQTT third-party libs (UniversalTelegramBot, AsyncMqttClient)
   sometimes return `String` from public API. Keep String at the
   handler boundary; convert immediately.

## Estimate

* Phase 0: done.
* Phases 1-2: ~half day each (mostly mechanical, ~20-30 files).
* Phase 3-4: ~half day each (call-site sweeps + tests).
* Phase 5: ~full day (framework headers + recompile everything).
* Phase 6: ~half day.

**Total: 3-4 dev days.** Spread across multiple sessions.

## Roll-back

Each phase is one commit. If soak regresses after phase N, revert that
commit only — earlier phases are isolated. No phase touches another
phase's file boundary except for Phase 5 (intentionally).
