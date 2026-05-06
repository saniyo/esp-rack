#ifndef Features_h
#define Features_h

#define FT_ENABLED(feature) feature

// project feature off by default
#ifndef FT_PROJECT
#define FT_PROJECT 0
#endif

// security feature on by default
#ifndef FT_SECURITY
#define FT_SECURITY 1
#endif

#ifndef FT_WEBSOCKET
#define FT_WEBSOCKET 1
#endif

// mqtt feature on by default
#ifndef FT_MQTT
#define FT_MQTT 1
#endif

// ntp feature on by default
#ifndef FT_NTP
#define FT_NTP 1
#endif

// mqtt feature on by default
#ifndef FT_OTA
#define FT_OTA 1
#endif

// upload firmware feature off by default
#ifndef FT_UPLOAD_FIRMWARE
#define FT_UPLOAD_FIRMWARE 0
#endif

#ifndef FT_TELEGRAM
#define FT_TELEGRAM 0
#endif

#ifndef FT_AUTO_UPDATE
#define FT_AUTO_UPDATE 0
#endif

#ifndef FT_SYSTEM_INFO
#define FT_SYSTEM_INFO 0
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
