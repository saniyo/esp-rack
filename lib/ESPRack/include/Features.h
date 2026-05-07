#ifndef Features_h
#define Features_h

// FT_ENABLED expansion — used at preprocessor time to gate code via
// `#if FT_ENABLED(FT_FOO)`. Each FT_* macro resolves to 0 or 1 from
// either a -D override in the consumer's features.ini OR the default
// below.
#define FT_ENABLED(feature) feature

// ── Default policy ──────────────────────────────────────────────────
// ESPRack ships as a single library bundle with all 19 modules always
// linked. Defaults therefore turn EVERY framework feature ON — a
// consumer that wants the whole framework just adds its own service
// (FT_PROJECT) and uses everything else with zero ceremony. To trim
// flash, the consumer explicitly disables what they don't need:
//
//   [features]
//   build_flags =
//     -D FT_PROJECT=1
//     -D FT_TELEGRAM=0   ; opt out of Telegram
//     -D FT_OTA=0        ; opt out of OTA
//
// FT_PROJECT stays OFF by default — the consumer's own service is
// the only thing the library can't predict. FT_SECRETS_VAULT stays
// ON for safety (credentials encrypted at rest unless explicitly
// disabled for debugging).

#ifndef FT_PROJECT
#define FT_PROJECT 0           // consumer's own service — opt-in
#endif

#ifndef FT_SECURITY
#define FT_SECURITY 1          // login + JWT + password hashing
#endif

#ifndef FT_WEBSOCKET
#define FT_WEBSOCKET 1         // WS infrastructure for live tabs
#endif

#ifndef FT_MQTT
#define FT_MQTT 1              // MQTT broker connection
#endif

#ifndef FT_NTP
#define FT_NTP 1               // NTP clock sync
#endif

#ifndef FT_OTA
#define FT_OTA 1               // ArduinoOTA over WiFi
#endif

#ifndef FT_UPLOAD_FIRMWARE
#define FT_UPLOAD_FIRMWARE 1   // POST .bin via web UI
#endif

#ifndef FT_TELEGRAM
#define FT_TELEGRAM 1          // Telegram bot integration
#endif

#ifndef FT_AUTO_UPDATE
#define FT_AUTO_UPDATE 1       // Periodic OTA poll against update server
#endif

#ifndef FT_SYSTEM_INFO
#define FT_SYSTEM_INFO 1       // /rest/systemStatus + System tab roll-up
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
