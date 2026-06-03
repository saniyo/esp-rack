#pragma once
#ifndef ESPRACK_IWIREGUARD_PROVIDER_H
#define ESPRACK_IWIREGUARD_PROVIDER_H

// IWireguardProvider — public read/control contract for the
// on-demand WireGuard tunnel. The provider owns:
//
//   • Curve25519 keypair (generated on first boot, persisted in
//     SecretsVault) — public key shared with mothership during
//     enrollment, private key never leaves the device.
//   • Tunnel netif state (down at boot; brought up only when
//     MothershipService dispatches an `openTunnel` action with the
//     server's endpoint+pubkey+assigned IP triplet).
//
// MothershipService is the only consumer in core ESP-Rack; UI tabs
// can additionally render status via the read-only getters. Mirrors
// ICertProvider / IMothershipProvider patterns.

#include <Arduino.h>

class IWireguardProvider {
 public:
  virtual ~IWireguardProvider() = default;

  // ── Identity ──
  // Device's WG public key, base64 (44 chars). Empty string before
  // the first keypair generation (rare — happens on boot before
  // SecretsVault is unlocked). The PRIVATE key never leaves the
  // device, no getter for it.
  //
  // Returned by const-ref to the provider's stable internal storage to
  // avoid a per-call heap copy (P3); valid until the next WG state
  // mutation — copy it if you need to hold it longer.
  virtual const String& publicKey() const = 0;

  // ── Runtime state ──
  // True if the tunnel netif is currently up AND the most recent
  // handshake is fresh (within keepalive window). False otherwise:
  // tunnel never brought up, brought down by closeTunnel, or peer
  // unreachable / handshake stalled.
  virtual bool isUp() const = 0;

  // True if the device knows ALL THREE pieces required to dial the
  // tunnel: server pubkey, server endpoint, and the assigned tunnel
  // IP. False until the first successful enrollment+openTunnel
  // cycle. Mothership reads this bit on every check-in — when a
  // trusted device reports hasTriplet=false the server auto-
  // provisions a peer and queues an openTunnel action with the
  // fresh triplet, so the operator doesn't have to click anything
  // in the dashboard.
  virtual bool hasTriplet() const = 0;

  // Assigned tunnel IP given by the mothership during the most
  // recent openTunnel cycle, e.g. "10.99.0.7". Empty if the tunnel
  // has never been brought up (so AsyncWebServer doesn't try to
  // bind a non-existent address). Returned by const-ref to stable
  // internal storage (P3); valid until the next WG state mutation.
  virtual const String& assignedIp() const = 0;

  // Seconds since the most recent successful handshake. INT32_MAX
  // when no handshake has ever happened on the current tunnel
  // session, or when the tunnel is down.
  virtual int32_t lastHandshakeAgeSec() const = 0;

  // Cumulative byte counters since the tunnel last came up. Reset
  // on every up() cycle. Useful for "idle" detection on the device
  // side (we currently rely on the server's idle timer, but having
  // the data here is cheap).
  virtual uint32_t txBytes() const = 0;
  virtual uint32_t rxBytes() const = 0;

  // ── Control surface ──
  //
  // Bring the tunnel up using the triplet supplied by the
  // mothership in the openTunnel action params. All three fields
  // are required; empty string means "use the value we cached from
  // the most recent enrollment / openTunnel". Returns true if the
  // tunnel netif came up (does NOT wait for the first handshake to
  // complete — caller polls lastHandshakeAgeSec for that).
  //
  // Idempotent: calling up() while already up with the same triplet
  // is a no-op. Calling up() with a different triplet triggers a
  // down()→up() rebuild.
  virtual bool up(const String& serverPublicKey,
                  const String& endpoint,
                  const String& assignedIp) = 0;

  // Bring the tunnel down, free the netif + per-peer state. After
  // this returns isUp() reports false and the device's
  // AsyncWebServer is reachable on its LAN IP only.
  virtual void down() = 0;
};

#endif  // ESPRACK_IWIREGUARD_PROVIDER_H
