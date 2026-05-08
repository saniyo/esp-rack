#pragma once
#ifndef ESPRACK_ICERT_PROVIDER_H
#define ESPRACK_ICERT_PROVIDER_H

// ICertProvider — public contract for CertManagerService. Consumer
// modules (Mothership client, future remote-debug, etc.) check
// `hasValidCert()` before assuming mTLS works, and read
// `daysUntilExpiry()` for status displays. Lifecycle mutations
// (enroll / rotate) live on the concrete service — this interface
// is read-mostly so consumers can poll without depending on the
// CertManagerService class directly.
//
// Mirrors IMqttProvider / ITelegramProvider conventions: live in
// modules/<name>/include/, App holds a pointer, late-bound by the
// owning Module's onInstall.

#include <Arduino.h>

class ICertProvider {
 public:
  virtual ~ICertProvider() = default;

  // Lifecycle state — surfaced in UI status fields.
  enum class State : uint8_t {
    Uninitialised,    // boot before begin() ran
    NeedsEnrollment,  // no cert on disk → operator must provide bootstrap token
    Enrolling,        // CSR submitted, awaiting server
    Ready,            // valid cert in storage, mTLS available
    Renewing,         // proactive rotate in progress (still using old cert)
    GrayZone,         // cert lost / expired offline → polling /recover, awaiting operator approve
    EnrollmentFailed, // last enrol returned error; will retry
  };

  virtual State state() const = 0;

  // True only when state == Ready (valid cert + key in TLSContextService).
  virtual bool hasValidCert() const = 0;

  // Days until current cert's notAfter. Negative if expired.
  // Returns INT32_MIN when no cert is loaded.
  virtual int32_t daysUntilExpiry() const = 0;

  // Cert subject CN (typically "device-<MAC>"). Empty when no cert.
  virtual const char* subjectCN() const = 0;

  // Cert serial as hex string (e.g. "0a:5f:..."). Empty when no cert.
  // Useful for operator-side device identification in audit logs.
  virtual const char* serialHex() const = 0;
};

#endif  // ESPRACK_ICERT_PROVIDER_H
