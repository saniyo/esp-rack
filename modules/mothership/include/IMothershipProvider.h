#pragma once
#ifndef ESPRACK_IMOTHERSHIP_PROVIDER_H
#define ESPRACK_IMOTHERSHIP_PROVIDER_H

// IMothershipProvider — public read-only contract for the mothership
// check-in client. Consumer modules / status panels query state via
// this interface without depending on the concrete service class.
// Mirrors ICertProvider conventions.
//
// Phase 2 only — Phase 3 (WireGuard tunnel) and Phase 4 (cert
// rotation) hook into the same service through additional internal
// APIs that the concrete class exposes.

#include <Arduino.h>

class IMothershipProvider {
 public:
  virtual ~IMothershipProvider() = default;

  // Lifecycle state — surfaced in UI status fields.
  enum class State : uint8_t {
    Disabled,        // operator turned off the module
    NeedsCert,       // CertManager hasn't enrolled yet — can't checkin
    Idle,            // armed, between check-ins
    CheckingIn,      // POST in flight
    LastOk,          // most recent check-in succeeded
    LastFail,        // most recent check-in failed (network / TLS / 5xx)
  };

  virtual State state() const = 0;

  // Seconds since the last successful check-in. INT32_MIN if never.
  virtual int32_t lastCheckInAgoSec() const = 0;

  // Seconds until the NEXT scheduled check-in (countdown). 0 if a
  // check-in is currently running, INT32_MIN if disabled.
  virtual int32_t nextCheckInInSec() const = 0;

  // Total successful check-ins since boot (for sanity / health).
  virtual uint32_t successCount() const = 0;
  virtual uint32_t failCount() const = 0;
};

#endif  // ESPRACK_IMOTHERSHIP_PROVIDER_H
