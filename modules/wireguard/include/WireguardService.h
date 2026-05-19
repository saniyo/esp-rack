#pragma once
#ifndef ESPRACK_WIREGUARD_SERVICE_H
#define ESPRACK_WIREGUARD_SERVICE_H

// WireguardService — concrete IWireguardProvider implementation.
//
// Lifecycle:
//   1. begin() loads persisted keypair from SecretsVault; if no
//      keypair yet, generates a fresh Curve25519 pair via mbedtls
//      and saves it. The pubkey becomes available to callers.
//   2. Tunnel stays DOWN at boot. MothershipService dispatches the
//      openTunnel action when an operator opens a device's UI panel
//      and calls up() with the triplet from the action params.
//   3. up() configures the WireGuard-ESP32-Arduino lib's WireGuard
//      object with the device's private key + server's pubkey +
//      assigned IP + endpoint, and starts the handshake. Tunnel
//      goes "up" once the lib reports handshake complete (polled
//      in loop()).
//   4. down() tears the lib's WireGuard down + clears the cached
//      triplet (next up() requires a fresh openTunnel from server).
//
// All public state mutations go through the IWireguardProvider
// virtuals; the form / WS tab reads via the same getters so the
// operator sees identical numbers to whatever programmatic code
// uses.

#include <StatefulService.h>
#include <ConfigManager.h>
#include <ConfigDelegate.h>
#include <FormBuilder.h>
#include <WebFeatureDelegate.h>

#include "IWireguardProvider.h"

#include <Arduino.h>

#define WG_FILE      "/config/wireguard.json"
#define WG_FORM_PATH "/rest/wireguard"

class WebManager;

struct WireguardSettings {
  // ── Persisted secret-key material ──
  // Curve25519 private key, raw 32 bytes base64-encoded (44 chars).
  // Encrypted at rest by SecretsVault (auto-discovered as a secret
  // because the key name matches the "*key" / "*token" pattern the
  // vault watches).
  String   priv_key;

  // ── Persisted public material ──
  // Derived from priv_key once at generation time and cached so we
  // don't recompute on every status read.
  String   pub_key;

  // Cached triplet from the most recent openTunnel action. Server
  // re-supplies it every cycle (so device doesn't strictly need to
  // persist), but we keep it here so the "Tunnel" tab can show the
  // last-known assigned IP / endpoint even when tunnel is down.
  String   server_pub_key;
  String   endpoint;            // "host:port" or "1.2.3.4:51820"
  String   assigned_ip;         // e.g. "10.99.0.7"

  // ── Runtime state (not persisted) ──
  bool     is_up{false};
  uint32_t up_since_ms{0};
  uint32_t last_handshake_ms{0};
  uint32_t tx_bytes{0};
  uint32_t rx_bytes{0};

  static void readConfig(WireguardSettings& s, JsonObject& root);
  static StateUpdateResult update(JsonObject& root, WireguardSettings& s);
  static void buildForm(WireguardSettings& s, JsonObject& root);
  static void staRead(WireguardSettings& s, JsonObject& root);
  static StateUpdateResult staUpd(JsonObject& root, WireguardSettings& s);
};

class WireguardService : public StatefulService<WireguardSettings>,
                          public IWireguardProvider {
 public:
  explicit WireguardService(ConfigManager* cfgMgr);

  void registerManifest(WebManager* web);
  void begin();
  void loop();

  // ── IWireguardProvider ──
  String   publicKey() const override        { return _state.pub_key; }
  bool     isUp() const override             { return _state.is_up; }
  String   assignedIp() const override       { return _state.assigned_ip; }
  int32_t  lastHandshakeAgeSec() const override;
  uint32_t txBytes() const override          { return _state.tx_bytes; }
  uint32_t rxBytes() const override          { return _state.rx_bytes; }

  bool     up(const String& serverPublicKey,
              const String& endpoint,
              const String& assignedIp) override;
  void     down() override;

 private:
  ConfigDelegate<WireguardSettings>   _cfg;
  WebFeatureEntry<WireguardSettings>* _feature{nullptr};

  // Lazy-generate the keypair the first time begin() runs without
  // one in SecretsVault. Idempotent on subsequent boots.
  void   ensureKeypair();

  // Curve25519 keypair generation via mbedtls — produces 32-byte
  // raw priv + 32-byte raw pub, base64-encoded for storage.
  // Returns true on success.
  static bool generateKeypair(String& outPrivB64, String& outPubB64);

  // Helper — base64 encode a fixed-size byte buffer using the
  // strict WG alphabet (the one mbedtls_base64_encode produces).
  static String base64Encode(const uint8_t* data, size_t len);
};

#endif  // ESPRACK_WIREGUARD_SERVICE_H
