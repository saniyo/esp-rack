# ESPRack Fleet Discovery — Final Protocol Decision & Spec

**Status:** DECIDED · **Date:** 2026-06-23
**Supersedes:** the `mdns_query_ptr`-browse path in `DiscoveryService` (esp-rack/modules/discovery)
**Audience:** implementer. This is a decision/spec doc, not code.
**Origin:** multi-agent design pass (4 research → 4 candidates → judge → synthesis). Judge scores: B(SWIM) 7.91 · C(hybrid) 7.00 · D(CoAP-RD) 5.83 · A(beacon) 5.25.

---

## 1. RECOMMENDATION

**Adopt a SWIM-style unicast-gossip membership protocol (Candidate B) as the endpoint, reached via a staged C→B rollout.** Evolve `DiscoveryService` from an mDNS browser into a unicast-probe failure detector over a fixed BSS peer table, with multicast demoted to a rare, HMAC'd, NVS-seeded **bootstrap-only** seed.

**Three-line why (50+ / autonomous / pull-aggregator):**
1. **50+ on lossy WiFi:** SWIM moves liveness off multicast onto all-unicast, 802.11-ACKed paths; a node dies only if a direct probe **and** all k=3 indirect probes fail (wrong-death ≈ p⁴, vs ~p for one multicast miss). Per-node load is O(1), independent of N. This is the only candidate that satisfies the multicast-loss constraint *structurally* rather than by widening timeout windows.
2. **Autonomous when the server is down:** state converges coordinator-free via incarnation-numbered gossip + 30 s anti-entropy; every node holds the same table, so the WG entry has **no SPOF** — any reachable device serves an equivalent table.
3. **Pull-aggregator:** the anti-entropy message **is** the server's pull endpoint (`GET /api/v1/discovery/table`). It is free — already built for gossip reconciliation.

### Do we drop mDNS, or keep it for `.local` resolution?

**Keep `MdnsModule` installed, but tear down the RESPONDER while keeping the client-side resolver path.** This is the load-bearing decision and the "sneaky blocker" — grounded in the real code, confirmed:

- `FACTORY_MOTHERSHIP_BASE_URL = "https://mothership.local:8443"` (MothershipService.h:41). The mothership URL is resolved through **lwIP's `gethostbyname()` `.local` hook**, which only functions **after `MDNS.begin()` has run** (the responder init). `MothershipService` does NOT call `IMdnsResolver::resolveHost` — it hands `mothership.local` to the TLS client and relies on the lwIP hook. **If you `MDNS.end()` and never `MDNS.begin()`, `mothership.local` stops resolving and checkin breaks.**
- `OTASettingsService.cpp:148` calls `app->mdns()` to advertise `<host>.local` for IDE/PlatformIO OTA — also responder-coupled.
- `IMdnsResolver.h:13` documents that `.local` auto-resolution requires `MDNS.begin()` was called.

**Pick ONE before ripping out the responder (hard prerequisite, not a follow-up):**

- **Option R1 (recommended, smallest blast radius):** keep `MDNS.begin()` running for **client queries only** (`queryHost`/`gethostbyname` hook), but **stop advertising** — remove `addService("_esprack",…)` + the three `addServiceTxt` calls in `MdnsService::initResponder()` (lines 251–257) and the `DiscoveryService::ensureAdvert()` TXT writes. **Empirically verify** (step-0) that `MDNS.begin()` with *zero advertised services* frees most of the ~12–15 KB — the cost is the *responder* (record lists + the unbounded TX-action queue, esp-idf#13333), and the TX-action queue only grows when you **answer incoming queries**, i.e. when you advertise. A query-only mDNS that advertises nothing should not run the responder TX path. **If the measurement shows bare `MDNS.begin()` still costs too much, fall back to R2.**
- **Option R2 (clean but more churn):** drop the responder entirely and **move the mothership URL off `.local`** — to the WireGuard endpoint IP, a static IP, or a server-provided IP delivered in the checkin response. Then `MdnsModule` can be fully removed and `DiscoveryModule`'s `requires_={"mdns"}` dropped. Frees the full responder unconditionally but forces a config/UX change (operators lose `mothership.local`) and breaks `.local` OTA.

**Decision: go R1 first; the step-0 measurement (§7) is the go/no-go that decides whether R1 suffices or you must escalate to R2.** Do not delete `MdnsModule` blindly — that is how you brick `mothership.local`.

### Staging (why C-then-B, not B directly)

The data planes of C (heartbeat+anti-entropy) are a **strict subset** of B (binary record, fixed BSS table, packet HMAC, REST pull, anti-entropy=pull). Build C's heartbeat + anti-entropy + BSS/HMAC/PULL first to de-risk the heap and serialization work in isolation (gated on step-0). Then, when the 50-node field test exposes C's multicast-liveness SUSPECT churn (its known weakness — §3.4), upgrade the failure detector to B's unicast k-relay + incarnation. **No work is wasted** — every C artifact carries into B.

---

## 2. WIRE PROTOCOL SPEC

### 2.1 Transport planes

| Plane | Transport | Endpoint | Rate | Role |
|---|---|---|---|---|
| **SEED** | UDP multicast, `IP_MULTICAST_TTL=1`, `LOOP=0`, egress pinned to STA netif | group `239.42.7.1` : port **7711** | 10 s ±3 s jitter, **gated** (only while `alive_count<2` or `boot_age<60 s`) | cold-join: learn ≥1 live peer. Loss-tolerant by design. |
| **MEMBERSHIP** | UDP **unicast** | `peer.ip:7711` | ~1 probe + ~1 ack / 2 s; ≤3 gossip / 500 ms | failure detection + piggybacked dissemination. 802.11-ACKed. |
| **ANTI-ENTROPY / PULL** | TCP/HTTP (reuse AsyncWebServer) | `GET /api/v1/discovery/table` | 30 s ±10 s to a random peer | full-table reconcile; **doubles as the server pull endpoint** |

One UDP socket, one fixed port 7711: `socket(AF_INET,SOCK_DGRAM)`, `SO_REUSEADDR`, `bind(INADDR_ANY:7711)`, `IP_ADD_MEMBERSHIP{239.42.7.1, STA-IP}`, `IP_MULTICAST_IF=STA`, `IP_MULTICAST_TTL=1`, `IP_MULTICAST_LOOP=0`. Both planes share it; demux on the `type` byte. **Re-issue `IP_ADD_MEMBERSHIP` every 60–120 s** to survive IGMP-snooping prune. RX into ONE static BSS buffer; keep every datagram ≤ 1 pbuf (≤ ~1400 B) so IP reassembly (10-pbuf budget) never triggers — zero malloc on the RX path above the socket.

### 2.2 Common header (14 B, little-endian, packed)

```
off sz  field    notes
0   1   magic    0xE5
1   1   ver      0x02  (v2=SWIM; v1 was the never-on-wire mDNS era)
2   1   type     1 PING · 2 ACK · 3 PING_REQ · 4 GOSSIP · 5 SEED · 6 SEED_ACK
3   1   flags    bit0 has_piggyback · bit1 req_ack · bit2 seed · bit3 leaving(graceful-bye)
4   6   src_id   sender STA MAC6 (raw bytes; = DeviceIdentity::macBytes())
10  4   seq      per-sender monotonic uint32 (replay window key)
```
Then a type-specific body, then the trailer:
```
N   4   epoch    sender boot-epoch (NVS-persisted counter, ++ each boot)
N+4 8   hmac     first 8 B of HMAC-SHA256(PSK, header‖body‖epoch) over RAW bytes
```

`epoch` defeats cross-reboot replay (seq resets to 0 on reboot; a higher epoch invalidates any captured pre-reboot packet). HMAC truncated to 8 B — enough for a LAN trust gate, keeps datagrams tiny.

### 2.3 Member record (32 B packed — the piggyback/table unit)

```
sz  field        notes
6   id           MAC6 (table key; identity root from DeviceIdentity)
4   ip           IPv4 (network order)
1   role         enum (light/sensor/relay/…); maps to the role menu (commit 765ba34)
1   state        0 alive · 1 suspect · 2 dead
2   caps         capability bitfield
4   fw           packed maj.min.patch.build (DeviceIdentity::version())
4   incarnation  SWIM refutation counter (per-id monotonic)
4   epoch        that member's boot-epoch (tiebreak before incarnation)
2   schema       manifest schema (DISCOVERY_SCHEMA_VERSION, "3" → 0x0003)
4   last_seen_rel age in seconds at send time; receiver re-bases to local clock
```
32 B/record. A 1400 B datagram holds ~36 records → a full 50-node delta fits in 2 datagrams; an all-50 digest fits in one.

**Anti-clone anchor:** the wire key is MAC6, but the BSS row also stores `uid4` (eFuse `DeviceIdentity::uidBytes()[0:4]`) learned from the full record / pull, so a MAC-spoof is still distinguishable. The human `canonical()` string (`<project>-<mac12>-<uid8>`) is reconstructed **only on the async serve path**, never on the RX hot path — the determinism win over today's six-`String`-per-peer `Peer` struct.

### 2.4 Message bodies

| type | body | purpose |
|---|---|---|
| `1 PING` | `[count:1] + records` (req_ack set) | direct liveness probe |
| `2 ACK` | optional piggyback | reply to PING / relayed PING_REQ |
| `3 PING_REQ` | `target_id:6 + piggyback` | "probe target for me" (k indirect relay) |
| `4 GOSSIP` | `count:1 + records` | dedicated dissemination (no probe semantics) |
| `5 SEED` | one record (self), flags.seed=1, multicast | bootstrap hello |
| `6 SEED_ACK` | digest records (id+ip+incarnation, 12 B each) | unicast reply to a SEED so a newcomer fills fast |

### 2.5 Sequence / replay

Per row: `last_seq:u32`, `last_epoch:u32`. Accept iff `epoch > last_epoch`, OR (`epoch == last_epoch` AND `seq` is within a forward window of `last_seq`, i.e. `(seq − last_seq) mod 2³² ∈ (0, 2³¹)`). **HMAC verified before any state mutation.** Replays/forgeries fail HMAC (no PSK) or the seq/epoch monotonic test → dropped, debug-log rate-limited.

---

## 3. MEMBERSHIP + FAILURE DETECTION

### 3.1 States

```
learn(seed/gossip/AE) ─────────────► ALIVE
ALIVE  ──direct PING fail + all k=3 PING_REQ fail──► SUSPECT  (gossip suspect)
SUSPECT ──record with higher incarnation = Alive──► ALIVE     (refutation)
SUSPECT ──suspicion timeout, no refute──► DEAD             (gossip confirm)
DEAD   ──DeadGossipTime expires──► slot freed
ANY    ──record with flags.leaving=1──► DEAD immediately  (graceful-bye)
```

**Merge/ordering rule (coordinator-free):** for member X a record wins by `epoch >`, else `incarnation >`, else state precedence `Alive < Suspect < Dead`. Only X may raise X's own incarnation, so X always wins its own refutation. `epoch` ensures a rebooted X (incarnation reset to 0) is not shadowed by its own pre-reboot Alive record.

### 3.2 Probe cycle (one period T)

1. Round-robin over a shuffled index; pick next ALIVE/SUSPECT `target`.
2. Unicast `PING(req_ack)` to `target.ip`, piggyback pending deltas; start ProbeTimeout.
3. `ACK` → ALIVE, `last_seen=now`, merge. Done.
4. Timeout → pick k random ALIVE peers, send each `PING_REQ{target}`; each relays a direct PING and forwards any ACK back. **Any** relayed ACK → ALIVE (loss was on our direct path; this also defeats *asymmetric* reachability — the case C cannot close).
5. Direct AND all k indirect fail → SUSPECT; set `suspect_deadline`; gossip it; **buddy-unicast** the SUSPECT straight to `target` so it can refute fastest.

### 3.3 Concrete timers (relaxed from memberlist LAN defaults for WiFi + single-core C3)

| Param | memberlist LAN | **ESPRack** | Why |
|---|---|---|---|
| ProbeInterval T | 1 s | **2 s** | C3 shares CPU with AsyncTCP(6144)+REST; halves probe CPU/airtime; detection still N-independent |
| ProbeTimeout | 500 ms | **900 ms** | WiFi RTT/jitter/AP buffering > wired; 500 ms false-positives under congestion |
| IndirectTimeout | — | **1500 ms** | bound the indirect round inside one T |
| IndirectChecks k | 3 | **3** | wrong-death ≈ p⁴ |
| GossipInterval | 200 ms | **500 ms** | plenty for O(log N) at N=50; saves loop time |
| GossipNodes (fanout) | 3 | **3** | |
| RetransmitMult | 4 | **4** | each delta retransmitted ~8× at N=50 |
| SuspicionMult | 4 | **5** | suspicion ≈ 5·log₁₀(N+1)·T ≈ 17 s at N=50; conservative vs WiFi blips |
| PushPull (anti-entropy) | 30 s | **30 s ±10 s** | jittered so 50 nodes don't sync-storm |
| SeedInterval | — | **10 s ±3 s, gated** | only while `alive<2` or `boot_age<60 s` |
| DeadGossipTime | 30 s | **30 s** | late nodes still learn the eviction |
| IGMP rejoin | — | **60–120 s** | survive snooping prune |

Detection ≈ 1.6·T ≈ ~3.2 s to first suspect; ~17 s suspicion → DEAD in ~20 s worst case. The server reasons on `state`+`last_seen`+`incarnation`, so instant DEAD is unnecessary.

### 3.4 How it survives WiFi multicast loss at 50+ (the load-bearing argument)

- **Multicast is off the steady-state path** — it carries only the gated SEED. Liveness rides **unicast** UDP, which gets 802.11 link-layer ACK + retransmit + normal data rate. The lossy primitive can never cause a false death.
- **Structural loss redundancy:** k=3 indirect probing makes a wrong verdict require independent failure of 4 paths. C, by contrast, keeps multicast as the *primary* death signal and accepts transient SUSPECT churn + an asymmetric-reachability gap a directed US→X probe can't close — which is exactly why B is the endpoint and C only the de-risking stage.
- **Anti-entropy backstop:** every 30 s two random nodes exchange the full table, deterministically repairing anything gossip lost — and this is what makes "any device serves the table" true.

### 3.5 Lifeguard (mandatory on the single-core C3 — not a follow-up)

Track a **Local Health Multiplier** `nsa ∈ [1..8]`: ++ when we miss an ACK we owed or blow our own probe deadline (loop stalled flushing a 6–8 KB REST form); −− on a clean period. Multiply `ProbeTimeout`, `IndirectTimeout`, and `suspect_deadline` by `nsa`. A momentarily-overloaded C3 becomes both slower to accuse and slower to be accused → kills the false-positive storm a busy single-core would otherwise emit. Two counters + a clamp, zero alloc. **Without it, REST bursts WILL produce false suspicions.**

### 3.6 GRAFT — correlated-loss circuit-breaker (from C/D)

B's p⁴ robustness assumes **independent** loss. A fleet-wide AP stall fails the direct probe AND all k indirect probes together → false mass-death — the one correlated-loss case Lifeguard alone doesn't fully cover. **Mitigation:** if **>30 % of the table goes SUSPECT within one window, freeze DEAD transitions** until the suspect fraction recedes. 15 simultaneous deaths is almost certainly a local AP problem, not 15 dead peers. ~20 LOC, decisive against the worst realistic failure.

### 3.7 GRAFT — graceful-bye (from A)

On planned reboot/`down()`/OTA, send **2 frames with `flags.leaving=1`**; peers evict instantly instead of waiting the full ~20 s suspicion window. Crashes are then the **only** case that pays the full DEAD window. Cheap, large staleness win for the common reboot/OTA case.

---

## 4. MEMORY MODEL

### 4.1 Fixed BSS table

```c
struct PeerRow {                 // packed, ~48 B
  uint8_t  id[6];                // MAC6 (key)
  uint8_t  uid4[4];              // eFuse anti-clone anchor
  uint32_t ip;
  uint8_t  role, state, _pad0, _pad1;
  uint16_t caps, schema;
  uint32_t fw, incarnation, epoch;
  uint32_t last_seen_ms;         // local millis(), re-based from wire
  uint32_t suspect_deadline_ms;  // 0 unless SUSPECT
  uint32_t last_seq, last_epoch; // replay window
  uint8_t  used;
};
static PeerRow s_table[ESPRACK_DISCOVERY_MAX_PEERS];   // .bss
static uint8_t s_shuffle[ESPRACK_DISCOVERY_MAX_PEERS]; // round-robin order
static uint8_t s_rxbuf[1400], s_txbuf[1400];           // one each
```

Open-addressed by `id` hash (or linear scan — 64 rows is trivial). **Zero steady-state malloc**; rows reused in place; Fisher–Yates over `s_shuffle`.

### 4.2 Sizing (recommend `MAX_PEERS = 64` — headroom over 50)

| N (slots) | table | + shuffle | + 2×1400 rx/tx | **total BSS** |
|---|---|---|---|---|
| 50 | 2.4 KB | 50 B | 2.8 KB | **~5.3 KB** |
| 64 | 3.1 KB | 64 B | 2.8 KB | **~6.0 KB** |
| 128 | 6.1 KB | 128 B | 2.8 KB | **~9.0 KB** |

### 4.3 Heap budget vs the ~12–15 KB freed

- Removing the **responder** (R1: stop advertising; R2: full removal) frees the responder state + service/PTR/SRV/TXT malloc'd lists **and the unbounded TX-action queue** (esp-idf#13333) — the real `max_alloc` determinism killer. It lives on the **heap**, so removing it **raises max_alloc**.
- The new ~6 KB table is **BSS** — it shrinks the heap *region* (lowers the ceiling slightly) but does NOT fragment and is invisible to `max_alloc`-as-largest-free-block the way a malloc'd hole is. Same P4 pattern the current code already cites (Mothership/Telegram BSS scratch).
- Today's `std::vector<Peer>` of up to 8 `String`-bearing rows churned **every 20 s** — deleted; that per-cycle heap traffic that touched `max_alloc` is gone.
- The discovery task stack stays **static** (`xTaskCreateStatic`, currently 3072 B / BSS). Binary parse needs *less* stack than the `mdns_query_ptr` browse it replaces → keep 3072 or trim to 2560 after measuring high-water. Task-count neutral (also delete the browse).
- **lwIP note:** `CONFIG_LWIP_UDP_RECVMBOX_SIZE` defaults to **6**; a 50-node gossip burst can silently drop datagrams. SWIM redundancy masks single drops, but **measure the drop counter and bump to 12–16** if it climbs (pointer-sized slots, not heap).

**Net:** trade ~6 KB deterministic BSS to free ~12–15 KB heap + kill an unbounded-growth path → the C3 target (`max_alloc > ~10 KB` so 6–8 KB REST form buffers don't 503) is met with margin — **gated on the step-0 measurement (§7).**

---

## 5. AGGREGATOR / SERVER BRIDGE

### 5.1 Endpoints (evolve the existing handler)

Keep `GET /api/v1/discovery/peers` (the React Discovery page; `{self, peers[]}`, `WebAuthLevel::Authenticated`) — re-point its body at the BSS table, reconstructing `device_id` from `id`/`uid4` on the async thread. Add:

```
GET /api/v1/discovery/table     ← server PULL (over WG) AND inter-node anti-entropy
  → { self:{device_id,ip,role,caps,fw,schema,incarnation,epoch},
      as_of_ms, n,
      peers:[{device_id,ip,role,caps,fw,schema,incarnation,epoch,state,last_seen_ms,age_ms}…] }
```

The **same handler** answers the server's WG pull and a peer's anti-entropy GET — anti-entropy is therefore free.

### 5.2 GRAFT — digest endpoint (from C)

```
GET /api/v1/discovery/digest
  → { node_count, alive, suspect, dead, table_hash, oldest_last_seen_ms }
```
Lets the server cheaply detect **non-convergence/staleness BEFORE pulling the full table**, and lets a node decide whether it even needs an anti-entropy round — **cuts unnecessary 6–8 KB JSON pulls on the C3**. Cheap, high-leverage.

### 5.3 Bounded serialization (no-PSRAM C3)

50 rows × ~130 B ≈ 6.5 KB JSON; a single contiguous ArduinoJson tree at 128 rows would dent `max_alloc`/503. **Use the chunked-generator (`beginChunkedResponse`)** already proven on C3 (the endpoints-table streaming path) — emit one peer per `advance()` pull, bounded ~256–512 B working buffer regardless of N. NOT `beginResponseStream` (it buffers). Optionally add `?fmt=bin` returning the packed 32 B records back-to-back (the AE payload itself) for a cheap server-side diff.

### 5.4 Any-device-can-serve, no SPOF, no elected coordinator

Every node converges to the same table (gossip + 30 s AE), so **any** reachable device serves an equivalent table. The WG-tunneled device (`IWireguardProvider::assignedIp()`) is the **natural entry, not a required one**; if it's down the server hits any other node's LAN IP. **No elected coordinator** — incarnation ordering gives coordinator-free convergence; introducing an RD/coordinator would reintroduce the forbidden SPOF (precisely why Candidate D's literal Resource Directory was rejected).

### 5.5 GRAFT — server-side union (from A & D)

During the eventual-consistency window, the server should **pull from 2–3 entry devices and union** the tables (and use `/digest` to spot a single node disagreeing with the rest). Operational safety net only — the protocol still converges; this just hides the transient.

### 5.6 Staleness contract (honest)

Convergence is **eventual**. A pull can show a SUSPECT row, a just-joined node missing for a few periods, or a just-departed node ALIVE until its ~20 s suspicion fires. The table carries `state` + `age_ms` + `incarnation` + `epoch` so **the server reasons about freshness instead of assuming a snapshot**. This is a behavioural change for the mothership (reason about state/age/epoch, not snapshots) — budget for it. Max staleness: new-join fleet-wide ≤ O(log N)·GossipInterval + worst-case 30 s AE; departure → DEAD ≤ ~20 s (instant with graceful-bye).

### 5.7 GRAFT (optional, only if interop is real) — `/.well-known/core` (from D)

Stream RFC 6690 CoRE Link-Format from the **same BSS table** via the chunked generator (~120 LOC, 0 heap): a pure interop veneer giving CoAP tooling / a future Thread-Matter bridge fleet visibility through any one node. **Not needed for the core goal — add only if interop is a stated requirement.**

---

## 6. SECURITY

- **Trust gate:** upgrade today's *static identity tag* `sig = hmacHex(PSK, canonical)` (DiscoveryService.cpp:98 — **fully replayable**, the code comment admits it) to a **per-PACKET** `HMAC-SHA256(PSK, header‖body‖epoch)` truncated to 8 B over **raw bytes** (not hex). Reuse the exact `mbedtls_md_hmac` path already in the file (stateless, stack `mac[32]`, **no mbedTLS context alloc on the hot path**). Authenticates membership AND integrity-protects every field.
- **Replay:** `epoch` (NVS boot counter, ++/boot) + per-sender `seq` window, both folded into the BSS row (**0 extra heap**). Captured packets from a prior boot fail the epoch test; in-session replays fail the seq monotonic test. No clock sync / NTP required.
- **Refutation abuse:** only the subject can raise its own incarnation, and refutation packets are themselves HMAC'd — the PSK is the trust boundary. Outside the PSK, no node can falsely keep a dead peer alive or evict a live one.
- **Bootstrap:** SEED/SEED_ACK are HMAC'd too; a newcomer ignores any seed failing HMAC. Rate-limit SEED processing (~10/s) to bound a flood.
- **Pull endpoint** stays `WebAuthLevel::Authenticated`, reached over WG/authenticated HTTP — not anonymous LAN.
- **PSK distribution** unchanged: `FACTORY_DISCOVERY_PSK` build def for bench; production = per-fleet PSK via mothership enroll.

---

## 7. IMPLEMENTATION PLAN (phased evolution of `DiscoveryService`)

**Shape:** keep `DiscoveryModule` (prio 24). `DiscoveryService::begin()` stops needing `IMdnsResolver*` for browsing and instead opens the UDP socket + starts the same static task. `registerManifest()` keeps `/peers`, adds `/table` + `/digest`. HMAC helper, mutex/serialise discipline, `DeviceIdentity` usage, static-task scaffold, and the chunked-response path are reused as-is.

### Phase 0 — the hard go/no-go GATE (do this FIRST)
1. **Resolve the `.local` coupling per §1 (R1 vs R2).** Stop advertising (R1) or move the mothership URL off `.local` (R2). **Confirm `mothership.local` still resolves and checkin still succeeds**, and that `.local` OTA either still works or is consciously dropped.
2. Build with the responder torn down per the chosen option + the ~6 KB BSS table linked but the **protocol idle**. **Measure idle `max_alloc` on the C3** (System → Resources / `HeapMonitor`). **Target: comfortably > ~10 KB**, ideally recovering toward the pre-mDNS idle ceiling. **If R1 doesn't clear the bar, escalate to R2 and re-measure. This gate decides whether the project proceeds.**

### Phase 1 — C subset (de-risk heap/BSS/HMAC/PULL in isolation)
3. `DiscoveryService.h`: replace `struct Peer`(6 Strings)+`vector<Peer>` with `PeerRow[CAP]` BSS + open-addressing helpers; add socket fd, seq/epoch, timers.
4. `DiscoveryService.cpp`: remove `ensureAdvert()`/`browseAndUpdate()`/`mdns_query_ptr`; add socket setup, packet codec (LE pack/unpack), per-packet HMAC sign/verify, multicast SEED + RX upsert, low-rate heartbeat, table_version-triggered anti-entropy.
5. Rewrite `servePeers()` over the BSS table (keep `sendAdaptiveJson` for `/peers`); add `/table` (chunked) + `/digest`.
6. **Build + flash 5-node bench** (use `flash-all.ps1` parallel pipeline): verify RX/upsert, SUSPECT/DEAD, graceful-bye, PULL table, replay rejection.

### Phase 2 — upgrade to B's failure detector (when the field test demands it)
7. Add unicast PING/ACK + PING_REQ(k=3) probe cycle; incarnation Suspect/Alive/Dead merge rule; **Lifeguard NSA** (mandatory); correlated-loss circuit-breaker; graceful-bye.
8. NVS-persisted seed peer IPs + mothership-seeded cold-join (server checkin response carries 1–2 seed IPs) for multicast-blocked APs.

### Phase 3 — operationalize
9. Server-side union across 2–3 entries; teach the mothership to reason about `state`/`age`/`epoch`/`digest`. Optional `/.well-known/core` only if interop is real.

**Keep vs remove:** KEEP `MdnsModule` (R1: responder-as-query-resolver only) OR remove it (R2, only after moving the mothership URL). REMOVE `DiscoveryService::ensureAdvert`/`browseAndUpdate` and `MdnsService::initResponder`'s `addService`/`addServiceTxt` advertising. **C3-first** verification throughout.

**Effort:** ~1.5–1.7 KLOC net new at the B endpoint (C subset is ~150–250 LOC lighter and lands first). Highest-effort candidate, honestly: no MCU SWIM port exists (the "ESP SWIM" hits are an unrelated STM8 debug protocol), empirical 50-node tuning required, Lifeguard in v1.

---

## 8. RISKS + OPEN QUESTIONS

**Risks (with mitigations):**
- **`.local` coupling is the real blocker, not the protocol** (confirmed: `mothership.local` + `.local` OTA via the lwIP responder hook). Mitigated by the §1 R1/R2 decision and the Phase-0 gate — **do not skip it.**
- **Cold-join when multicast is fully blocked** — SWIM doesn't self-bootstrap. Mandatory NVS seed IPs + mothership-seeded discovery (§7 Phase 2). Document AP levers (min multicast rate ≥12 Mbps, DTIM=1) as *guidance only*; correctness must not depend on them.
- **Correlated mass-death** (fleet-wide AP stall) — the one case k=3 doesn't cover. Mitigated by the §3.6 circuit-breaker + Lifeguard.
- **False positives on a busy C3** — Lifeguard NSA (§3.5), in v1.
- **Eventual consistency** — server must reason about `state`/`age`/`epoch`; server-side union + `/digest` as safety nets. A behavioural change for the mothership.
- **Empirical tuning** — §3.3 timers are educated starting points; the 50-node WiFi field test WILL require another round of ProbeTimeout/SuspicionMult adjustment.
- **`UDP_RECVMBOX_SIZE=6`** — measure drop counter; bump to 12–16 if it climbs.

**Open questions for the implementer:**
1. **R1 vs R2:** does a bare `MDNS.begin()` that advertises *nothing* free enough of the ~12–15 KB on the C3? (Phase-0 measurement answers this — and decides whether `mothership.local` can survive.)
2. Is `.local` OTA worth preserving, or is the WG/IP path acceptable for OTA so R2 becomes clean?
3. Is `MAX_PEERS=64` the right ceiling, or do you foresee a subnet > 64 ESPRack devices (→ 128 slots, ~9 KB BSS)?
4. Is CoAP interop (`/.well-known/core`) ever a real requirement, or skip the veneer?
5. Does the mothership team accept the state/age/epoch reasoning change, and will it pull from 2–3 entries for the union?

---

### Key source files (grounded in reads during the design pass)
- `esp-rack/modules/discovery/src/DiscoveryService.cpp` — evolve: `hmacHex` (40), static `_sig` tag (98), `browseAndUpdate` (148), `servePeers` (198, `sendAdaptiveJson(...,512)`), `DISCOVERY_MAX_PEERS=8`, static 3072 B task (110–119).
- `esp-rack/modules/discovery/include/DiscoveryService.h` — `struct Peer` (6 Strings) → `PeerRow` BSS.
- `esp-rack/modules/discovery/include/DiscoveryModule.h` — prio 24, `requires_={"mdns"}` (drop only under R2), `begin(app_->mdns())`.
- `esp-rack/modules/discovery/mdns/src/MdnsService.cpp` — `initResponder()` advertise calls (251–257) to stop; `resolveHost`/`queryHost` (266–285) to KEEP; the `.local` responder coupling.
- `esp-rack/modules/discovery/mdns/include/IMdnsResolver.h` — documents the `gethostbyname()` `.local` hook needs `MDNS.begin()`.
- `esp-rack/modules/mothership/include/MothershipService.h` — **`FACTORY_MOTHERSHIP_BASE_URL = "https://mothership.local:8443"` (41) — the sneaky blocker.**
- `esp-rack/modules/mothership/src/MothershipService.cpp` — checkin body `{deviceId=_cert->subjectCN(), fwVer, hwRev, …}` + `wg{pubkey,has_triplet,is_up}` (581, 620–625).
- `esp-rack/modules/wireguard/include/IWireguardProvider.h` — `assignedIp()` (58), `isUp()` (41) — WG pull entry.
- `esp-rack/modules/ota/src/OTASettingsService.cpp` — `app->mdns()` advertise for `.local` OTA (148): a second responder consumer under R2.
