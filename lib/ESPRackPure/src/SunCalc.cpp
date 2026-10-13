#include <ESPRackPure/SunCalc.h>

#include <math.h>

namespace ESPRack {
namespace astro {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kRad = kPi / 180.0;

// Flash budget: only sin, cos, atan2, sqrt and floor from libm (most
// firmwares already link sin/cos/floor); tan, asin, acos, fmod and ceil are
// expressed through them.
inline double rad(double d) { return d * kRad; }
inline double deg(double r) { return r / kRad; }
inline double modPos(double a, double m) { return a - m * floor(a / m); }
inline double clamp1(double v) { return v > 1.0 ? 1.0 : v < -1.0 ? -1.0 : v; }
inline double asinC(double x) {
  x = clamp1(x);
  return atan2(x, sqrt(1.0 - x * x));
}
inline double acosC(double x) {
  x = clamp1(x);
  return atan2(sqrt(1.0 - x * x), x);
}
inline double roundHalfUp(double v) { return floor(v + 0.5); }

struct Sun {
  double decl;       // radians
  double eqTimeMin;  // equation of time, minutes
};

// NOAA Solar Calculator, column by column.
Sun sunAt(double utcS) {
  const double jd = utcS / 86400.0 + 2440587.5;
  const double T = (jd - 2451545.0) / 36525.0;
  const double L0 = modPos(280.46646 + T * (36000.76983 + T * 0.0003032), 360.0);
  const double M = rad(357.52911 + T * (35999.05029 - 0.0001537 * T));
  const double e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
  const double C = sin(M) * (1.914602 - T * (0.004817 + 0.000014 * T)) + sin(2 * M) * (0.019993 - 0.000101 * T) +
                   sin(3 * M) * 0.000289;
  const double omega = rad(125.04 - 1934.136 * T);
  const double appLong = rad(L0 + C - 0.00569 - 0.00478 * sin(omega));
  const double eps0 = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
  const double eps = rad(eps0 + 0.00256 * cos(omega));
  const double cosEps = cos(eps);
  const double y = (1.0 - cosEps) / (1.0 + cosEps);  // tan²(eps/2)
  const double L0r = rad(L0);
  const double eqt = 4.0 * deg(y * sin(2 * L0r) - 2 * e * sin(M) + 4 * e * y * sin(M) * cos(2 * L0r) -
                               0.5 * y * y * sin(4 * L0r) - 1.25 * e * e * sin(2 * M));
  return Sun{asinC(sin(eps) * sin(appLong)), eqt};
}

struct Geo {
  double elevDeg;  // geometric
  double ha;       // hour angle, radians, negative before solar noon
  double decl;     // radians
};

// The one place elevation is computed (sunPosition and the crossing search).
Geo geometry(double utcS, double sinLat, double cosLat, double lonDeg) {
  const Sun s = sunAt(utcS);
  const double tst = modPos(modPos(utcS, 86400.0) / 60.0 + s.eqTimeMin + 4.0 * lonDeg, 1440.0);
  const double ha = rad(tst / 4.0 - 180.0);
  const double elev = deg(asinC(sinLat * sin(s.decl) + cosLat * cos(s.decl) * cos(ha)));
  return Geo{elev, ha, s.decl};
}

double refractionDeg(double e) {
  if (e > 85.0) return 0.0;
  const double te = sin(rad(e)) / cos(rad(e));
  double arcsec;
  if (e > 5.0) {
    arcsec = 58.1 / te - 0.07 / (te * te * te) + 0.000086 / (te * te * te * te * te);
  } else if (e > -0.575) {
    arcsec = 1735.0 + e * (-518.2 + e * (103.4 + e * (-12.79 + e * 0.711)));
  } else {
    arcsec = -20.772 / te;
  }
  return arcsec / 3600.0;
}

struct Crossing {
  double sinLat, cosLat, lon, h0;
  bool up;
  double f(double t) const { return geometry(t, sinLat, cosLat, lon).elevDeg - h0; }
  bool crosses(double fPrev, double fCur) const { return up ? (fPrev < 0 && fCur >= 0) : (fPrev >= 0 && fCur < 0); }
};

bool secant(const Crossing& c, double guess, double& root) {
  double t0 = guess - 30, t1 = guess + 30;
  double f0 = c.f(t0), f1 = c.f(t1);
  for (int i = 0; i < 3; ++i) {
    if (f1 == f0) return false;
    const double t2 = t1 - f1 * (t1 - t0) / (f1 - f0);
    t0 = t1;
    f0 = f1;
    t1 = t2;
    f1 = c.f(t1);
    if (fabs(t1 - t0) < 0.5) break;
  }
  if (fabs(f1) > 2e-3) return false;  // not converged (~1 s at mid latitudes)
  // right direction: rising through h0 for Ascending
  const double before = c.f(t1 - 60), after = c.f(t1 + 60);
  if (c.up ? !(before < 0 && after > 0) : !(before > 0 && after < 0)) return false;
  root = t1;
  return true;
}

bool scan(const Crossing& c, double from, double end, int64_t& out) {
  double tPrev = from, fPrev = c.f(from);
  while (tPrev < end) {
    double t = tPrev + 600.0;
    if (t > end) t = end;
    const double fCur = c.f(t);
    if (c.crosses(fPrev, fCur)) {
      double lo = tPrev, hi = t;  // crosses(f(lo), f(hi)) holds
      while (hi - lo > 0.5) {
        const double mid = 0.5 * (lo + hi);
        if (c.crosses(fPrev, c.f(mid))) {
          hi = mid;
        } else {
          lo = mid;
        }
      }
      out = static_cast<int64_t>(roundHalfUp(hi));
      return true;
    }
    tPrev = t;
    fPrev = fCur;
  }
  return false;
}
}  // namespace

SunPos sunPosition(int64_t utcS, double latDeg, double lonDeg, bool refraction) {
  const double latR = rad(latDeg);
  const double sinLat = sin(latR), cosLat = cos(latR);
  const Geo g = geometry(static_cast<double>(utcS), sinLat, cosLat, lonDeg);
  // atan2(sin H, cos H sin lat - tan d cos lat), both terms scaled by cos d > 0
  const double cd = cos(g.decl);
  double az = deg(atan2(sin(g.ha) * cd, cos(g.ha) * sinLat * cd - sin(g.decl) * cosLat)) + 180.0;
  if (az >= 360.0) az -= 360.0;
  const double elev = refraction ? g.elevDeg + refractionDeg(g.elevDeg) : g.elevDeg;
  return SunPos{elev, az};
}

int64_t solarNoon(int64_t nearUtcS, double lonDeg) {
  const double lmt = static_cast<double>(nearUtcS) + lonDeg * 240.0;  // local mean time, s
  const double dayStart = floor(lmt / 86400.0) * 86400.0 - lonDeg * 240.0;
  double noon = dayStart + 43200.0;
  for (int i = 0; i < 2; ++i) noon = dayStart + 43200.0 - sunAt(noon).eqTimeMin * 60.0;
  return static_cast<int64_t>(roundHalfUp(noon));
}

bool nextCrossing(int64_t fromUtcS, double latDeg, double lonDeg, double h0Deg, Dir dir, int64_t& out,
                  int32_t horizonS) {
  if (horizonS <= 0) return false;
  const double latR = rad(latDeg);
  const Crossing c{sin(latR), cos(latR), lonDeg, h0Deg, dir == Dir::Ascending};
  const double from = static_cast<double>(fromUtcS);
  const double end = from + horizonS;
  const double side = c.up ? -1.0 : 1.0;
  const double noon0 = static_cast<double>(solarNoon(fromUtcS, lonDeg));
  const double sinH0 = sin(rad(h0Deg)), sinLat = c.sinLat, cosLat = c.cosLat;

  // Fast path: the hour angle at which the sun reaches h0, day by day.
  for (int k = -1; k <= 2; ++k) {
    const double noon = noon0 + 86400.0 * k;
    double g = noon;
    bool ok = cosLat > 1e-9;
    for (int i = 0; ok && i < 2; ++i) {  // declination at the guess, refined once
      const Sun s = sunAt(g);
      const double ch = (sinH0 - sinLat * sin(s.decl)) / (cosLat * cos(s.decl));
      ok = ch >= -1.0 && ch <= 1.0;
      if (ok) g = noon + side * deg(acosC(ch)) * 240.0;
    }
    if (!ok) break;  // no such crossing that day: let the scan decide
    if (g < from - 1800.0) continue;
    if (g > end + 1800.0) break;
    double root = 0;
    if (!secant(c, g, root)) break;
    if (root < from) continue;
    if (root > end) break;
    out = static_cast<int64_t>(roundHalfUp(root));
    if (out < fromUtcS) out = fromUtcS;
    return true;
  }
  return scan(c, from, end, out);
}

}  // namespace astro
}  // namespace ESPRack
