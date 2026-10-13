#pragma once
#ifndef ESPRACK_GEO_ILOCATION_PROVIDER_H
#define ESPRACK_GEO_ILOCATION_PROVIDER_H

// Device location, late-bound like the other providers (App::location()).
// One module implements it (a "location" module: manual coordinates, last
// good GPS fix); a GPS module only feeds fixes in through submitGps(), so it
// never depends on the location module. Consumers read current() and
// subscribe() to changes; null-check App::location() — no provider means no
// location.

#include <stdint.h>

#include <functional>

namespace ESPRack {

enum class LocationSource : uint8_t { None, Manual, GpsLastGood, Gps };

struct GeoFix {
  bool valid;
  double latDeg, lonDeg;
  float altM, accuracyM;
  LocationSource src;
  int64_t fixUtcS;  // 0 = unknown
  uint64_t monoMs;  // when the fix was taken
};

class ILocationProvider {
 public:
  virtual ~ILocationProvider() = default;
  // The best location now (live GPS, else last good GPS, else manual).
  virtual bool current(GeoFix& out) const = 0;
  virtual bool manual(GeoFix& out) const = 0;
  // From a GPS module, loop task.
  virtual void submitGps(const GeoFix& fix) = 0;
  // onlyIfUnset: keep an operator-entered location (e.g. seeding from GPS).
  virtual bool setManual(double latDeg, double lonDeg, float altM, bool onlyIfUnset) = 0;
  virtual uint32_t revision() const = 0;  // bumps whenever current() changes
  virtual int8_t subscribe(std::function<void(const GeoFix&)> cb) = 0;  // -1 when full
  virtual void unsubscribe(int8_t id) = 0;
};

}  // namespace ESPRack

#endif  // ESPRACK_GEO_ILOCATION_PROVIDER_H
