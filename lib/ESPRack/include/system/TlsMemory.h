#pragma once
#ifndef ESPRACK_SYSTEM_TLS_MEMORY_H
#define ESPRACK_SYSTEM_TLS_MEMORY_H

// TlsMemory: where mbedTLS (WiFiClientSecure, MQTT over TLS, HTTPS) takes its
// memory. The Arduino libraries are built with CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC:
// every TLS connection takes ~35-45 KB of internal RAM, its receive buffer as
// one 16 KB block. On a device whose internal heap is fragmented the handshake
// then fails ("largest block 30 KB" is not enough), and each connection cuts
// the largest block for everyone else.
//
// With PSRAM, App::begin() points mbedTLS at an allocator that puts blocks of
// ESPRACK_TLS_PSRAM_MIN bytes and more into PSRAM (internal RAM if PSRAM is
// full) and keeps smaller ones internal (internal RAM first, then PSRAM).
// Without PSRAM nothing changes. ESP-IDF supports TLS buffers in PSRAM
// (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC); this is the same at run time.
// ESPRACK_TLS_PSRAM=0 turns it off.

#include <stddef.h>
#include <stdint.h>

#ifndef ESPRACK_TLS_PSRAM
#define ESPRACK_TLS_PSRAM 1
#endif
#ifndef ESPRACK_TLS_PSRAM_MIN
#define ESPRACK_TLS_PSRAM_MIN 1024
#endif

namespace ESPRack {
namespace TlsMemory {

// Once, before the first TLS connection (App::begin). True when mbedTLS now
// uses PSRAM for its large blocks.
bool begin();
bool usingPsram();

}  // namespace TlsMemory
}  // namespace ESPRack

#endif  // ESPRACK_SYSTEM_TLS_MEMORY_H
