#pragma once
#ifndef ESPRACK_PURE_SUN_CALC_H
#define ESPRACK_PURE_SUN_CALC_H

// Sun position and horizon crossings — the NOAA Solar Calculator equations
// (after Meeus, "Astronomical Algorithms"): ~0.01° for 1800..2100, well inside
// what dusk/dawn control needs. Pure, double precision, no tables.

#include <stdint.h>

namespace ESPRack {
namespace astro {

struct SunPos {
  double elevDeg;  // geometric unless refraction was asked for
  double azDeg;    // from north, clockwise
};

// refraction: add the NOAA atmospheric refraction approximation.
SunPos sunPosition(int64_t utcS, double latDeg, double lonDeg, bool refraction = false);

enum class Dir : uint8_t { Descending, Ascending };

// The first instant >= fromUtcS within horizonS at which the GEOMETRIC
// elevation crosses h0Deg in `dir` (-0.833 = sunrise/sunset with standard
// refraction and the solar semi-diameter, -6 civil, -12 nautical twilight).
// false when there is none inside the horizon (polar day / night).
// Analytic hour-angle guess + at most 3 secant steps; 10-minute scan +
// bisection fallback when the guess does not apply.
bool nextCrossing(int64_t fromUtcS, double latDeg, double lonDeg, double h0Deg, Dir dir, int64_t& out,
                  int32_t horizonS = 129600);

// Solar noon (hour angle 0) of the local mean solar day containing nearUtcS.
int64_t solarNoon(int64_t nearUtcS, double lonDeg);

}  // namespace astro
}  // namespace ESPRack

#endif  // ESPRACK_PURE_SUN_CALC_H
