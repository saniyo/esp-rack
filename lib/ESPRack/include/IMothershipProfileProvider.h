#pragma once
#ifndef ESPRACK_IMOTHERSHIP_PROFILE_PROVIDER_H
#define ESPRACK_IMOTHERSHIP_PROFILE_PROVIDER_H

// IMothershipProfileProvider — named "mothership" endpoint profiles
// owned by MothershipService and consumed by CertManagerService.
//
// Mirrors the consumer pattern of ITelegramProvider / ICertProvider:
// lives in lib/ESPRack/include/ as a forward-declared interface,
// App holds a pointer, MothershipModule's onInstall late-binds the
// concrete MothershipService into it. CertManager reads URLs through
// app->mothershipProfile()->...() at request time — never caches a
// snapshot — so a profile switch in the UI takes effect on the next
// HTTP call without restarting anything.
//
// Why a dedicated profile concept: a single device often needs to
// reach different mothership instances (home LAN dev box vs. work
// staging vs. production cloud) and editing three URL fields by
// hand across CertManager + Mothership settings tabs every time
// the operator moves between networks is friction we don't need.
// One profile carries the base URL; the three endpoint paths
// (/api/v1/enroll, /checkin, /recover) are fixed by server contract.

#include <Arduino.h>

class IMothershipProfileProvider {
 public:
  virtual ~IMothershipProfileProvider() = default;

  // Active profile's user-facing name ("Home" / "Work" / ...). Empty
  // when no profile is selected — in that case the URL getters
  // below return empty strings and consumers should treat the
  // mothership feature as disabled.
  virtual String activeName() const = 0;

  // Active profile's base URL ("https://192.168.88.5:8443"), no
  // trailing slash. Empty if no active profile.
  virtual String activeBaseUrl() const = 0;

  // Derived endpoint URLs. All three are
  //   <activeBaseUrl()> + "<fixed path>"
  // and return empty when no profile is active. Consumers MUST
  // null/empty-check before issuing HTTP — empty URL = profile not
  // configured, not a bug.
  virtual String enrollUrl()  const = 0;   // /api/v1/enroll
  virtual String checkinUrl() const = 0;   // /api/v1/checkin
  virtual String recoverUrl() const = 0;   // /api/v1/recover
};

#endif  // ESPRACK_IMOTHERSHIP_PROFILE_PROVIDER_H
