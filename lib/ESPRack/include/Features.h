#ifndef Features_h
#define Features_h

// FT_ENABLED expansion — used at preprocessor time to gate code via
// `#if FT_ENABLED(FT_FOO)`. Each FT_* macro resolves to 0 or 1 from
// either a -D override in the consumer's features.ini OR the default
// below.
#define FT_ENABLED(feature) feature

// ── Feature control policy ──────────────────────────────────────────
// MODULE features are turned on/off purely by the Builder().install<>()
// chain in the consumer's composition root (main.cpp). To drop a feature
// — Telegram, MQTT, NTP, OTA, Upload-Firmware, Auto-Update, System-Status,
// Cert-Manager, Mothership, the Discovery/mDNS pair, … — remove that
// module's .install<>() call (and its lib_deps entry to reclaim flash).
// The manifest's buildFeatures map (and /api/v1/features) is DERIVED from
// what is actually installed (App::begin walks the installed modules), so
// the frontend adapts with zero build flags and there can be no drift
// between macros and what was wired up. There are deliberately NO
// per-module FT_* macros.
//
// The FT_* flags that remain are NOT module toggles — they gate behavior
// that has no standalone module to not-install:
//
//   FT_PROJECT        the consumer's own service body (opt-in; OFF by
//                     default — the library can't predict the consumer
//                     service). Mute it by not installing the consumer
//                     module, or build with -D FT_PROJECT=0.
//   FT_SECURITY       swaps the core SecurityManager for
//                     NullSecurityManager (everyone authenticated as
//                     admin) — a posture toggle woven through the core,
//                     not a standalone module.
//   FT_WEBSOCKET      core WS infrastructure for live tabs.
//   FT_SECRETS_VAULT  field-level password encryption at rest (ON for
//                     safety; mbedtls AES/SHA-256 compiled out when 0).
//   FT_HEAP_MONITOR /
//   FT_LOOP_PROFILE   DEBUG-only instrumentation (ship OFF).
//
//   [features]
//   build_flags =
//     -D FT_PROJECT=1          ; enable the consumer's own service
//     -D FT_SECRETS_VAULT=0    ; plaintext secrets on disk (debug only)

#ifndef FT_PROJECT
#define FT_PROJECT 0           // consumer's own service — opt-in
#endif

#ifndef FT_SECURITY
#define FT_SECURITY 1          // login + JWT + password hashing
#endif

#ifndef FT_WEBSOCKET
#define FT_WEBSOCKET 1         // WS infrastructure for live tabs
#endif

// ── Diagnostics pair (DEBUG-only, ship OFF) ─────────────────────────
// Both were built for the perf / heap-fragmentation hunt. That work is
// closed, so they default OFF — flip to 1 in features.ini only while
// actively investigating a regression.
//
// FT_HEAP_MONITOR — HeapMonitor's per-minute max_alloc ring + boot / WG /
// mship / soak snapshot logging + per-task stack dump. When 0 the 960 B
// BSS ring and all logSnapshot/tick/dumpTasks call sites compile to
// no-ops (serial quiet, flash drops the tag strings); the System-tab
// "Heap @ idle" row still reads live heap via snapshot().
#ifndef FT_HEAP_MONITOR
#define FT_HEAP_MONITOR 0
#endif

// FT_LOOP_PROFILE — App::loop's once-a-minute per-module profiler
// (dt / dheap / dmax / dstack per module, ~25 log_i lines/min). When 0
// the instrumentation is compiled out and loop() runs the bare onLoop
// fan-out with zero per-iteration overhead.
#ifndef FT_LOOP_PROFILE
#define FT_LOOP_PROFILE 0
#endif

// Field-level password encryption (SecretsVault — AES-128-CBC with a
// chip-derived key). When FT_SECRETS_VAULT=0, the SecretsVault
// implementation is preprocessed out — mbedtls AES + SHA-256 + eFuse
// MAC reads are NOT linked, ConfigManager skips the encrypt/decrypt
// walk entirely, and password fields land on disk as plaintext. ON by
// default to keep credentials encrypted at rest.
#ifndef FT_SECRETS_VAULT
#define FT_SECRETS_VAULT 1
#endif

#endif
