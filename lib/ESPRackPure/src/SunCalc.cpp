#include <ESPRackPure/SunCalc.h>

#include <math.h>

namespace ESPRack {
namespace astro {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kRad = kPi / 180.0;

inline double rad(double d) { return d * kRad; }
inline double deg(double r) { return r / kRad; }
inline double fmodPos(double a, double m) {
  const double r = fmod(a, m);
  return r < 0 ? r + m : r;
}
inline double clamp1(double v) { return v > 1.0 ? 1.0 : v < -1.0 ? -1.0 : v; }

struct Sun {
  double decl;       // radians
  double eqTimeMin;  // equation of time, minutes
};

// NOAA Solar Calculator, column by column.
Sun sunAt(double utcS) {
  const double jd = utcS / 86400.0 + 2440587.5;
  const double T = (jd - 2451545.0) / 36525.0;
  const double L0 = fmodPos(280.46646 + T * (36000.76983 + T * 0.0003032), 360.0);
  const double M = rad(357.52911 + T * (35999.05029 - 0.0001537 * T));
  const double e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
  const double C = sin(M) * (1.914602 - T * (0.004817 + 0.000014 * T)) + sin(2 * M) * (0.019993 - 0.000101 * T) +
                   sin(3 * M) * 0.000289;
  const double omega = rad(125.04 - 1934.136 * T);
  const double appLong = rad(L0 + C - 0.00569 - 0.00478 * sin(omega));
  const double eps0 = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
  const double eps = rad(eps0 + 0.00256 * cos(omega));
  const double y = tan(eps / 2) * tan(eps / 2);
  const double L0r = rad(L0);
  const double eqt = 4.0 * deg(y * sin(2 * L0r) - 2 * e * sin(M) + 4 * e * y * sin(M) * cos(2 * L0r) -
                               0.5 * y * y * sin(4 * L0r) - 1.25 * e * e * sin(2 * M));
  return Sun{asin(sin(eps) * sin(appLong)), eqt};
}

// Hour angle in degrees, [-180, 180), negative before solar noon.
double hourAngle(double utcS, double lonDeg, const Sun& s) {
  const double tst = fmodPos(fmodPos(utcS, 86400.0) / 60.0 + s.eqTimeMin + 4.0 * lonDeg, 1440.0);
  return tst / 4.0 - 180.0;
}

double elevGeo(double utcS, double latR, double lonDeg) {
  const Sun s = sunAt(utcS);
  const double ha = rad(hourAngle(utcS, lonDeg, s));
  return deg(asin(clamp1(sin(latR) * sin(s.decl) + cos(latR) * cos(s.decl) * cos(ha))));
}

double refractionDeg(double e) {
  if (e > 85.0) return 0.0;
  const double te = tan(rad(e));
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
  double latR, lon, h0;
  bool up;
  double f(double t) const { return elevGeo(t, latR, lon) - h0; }
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
      out = static_cast<int64_t>(ceil(hi - 0.5));
      return true;
    }
    tPrev = t;
    fPrev = fCur;
  }
  return false;
}
}  // namespace

SunPos sunPosition(int64_t utcS, double latDeg, double lonDeg, bool refraction) {
  const double t = static_cast<double>(utcS);
  const Sun s = sunAt(t);
  const double latR = rad(latDeg);
  const double ha = rad(hourAngle(t, lonDeg, s));
  double elev = deg(asin(clamp1(sin(latR) * sin(s.decl) + cos(latR) * cos(s.decl) * cos(ha))));
  const double az = fmodPos(deg(atan2(sin(ha), cos(ha) * sin(latR) - tan(s.decl) * cos(latR))) + 180.0, 360.0);
  if (refraction) elev += refractionDeg(elev);
  return SunPos{elev, az};
}

int64_t solarNoon(int64_t nearUtcS, double lonDeg) {
  const double lmt = static_cast<double>(nearUtcS) + lonDeg * 240.0;  // local mean time, s
  const double dayStart = floor(lmt / 86400.0) * 86400.0 - lonDeg * 240.0;
  double noon = dayStart + 43200.0;
  for (int i = 0; i < 2; ++i) noon = dayStart + 43200.0 - sunAt(noon).eqTimeMin * 60.0;
  return static_cast<int64_t>(floor(noon + 0.5));
}

bool nextCrossing(int64_t fromUtcS, double latDeg, double lonDeg, double h0Deg, Dir dir, int64_t& out,
                  int32_t horizonS) {
  if (horizonS <= 0) return false;
  const Crossing c{rad(latDeg), lonDeg, h0Deg, dir == Dir::Ascending};
  const double from = static_cast<double>(fromUtcS);
  const double end = from + horizonS;
  const double side = c.up ? -1.0 : 1.0;
  const double noon0 = static_cast<double>(solarNoon(fromUtcS, lonDeg));
  const double sinH0 = sin(rad(h0Deg)), sinLat = sin(c.latR), cosLat = cos(c.latR);

  // Fast path: the hour angle at which the sun reaches h0, day by day.
  for (int k = -1; k <= 2; ++k) {
    const double noon = noon0 + 86400.0 * k;
    double g = noon;
    bool ok = cosLat > 1e-9;
    for (int i = 0; ok && i < 2; ++i) {  // declination at the guess, refined once
      const Sun s = sunAt(g);
      const double ch = (sinH0 - sinLat * sin(s.decl)) / (cosLat * cos(s.decl));
      ok = ch >= -1.0 && ch <= 1.0;
      if (ok) g = noon + side * deg(acos(ch)) * 240.0;
    }
    if (!ok) break;  // no such crossing that day: let the scan decide
    if (g < from - 1800.0) continue;
    if (g > end + 1800.0) break;
    double root = 0;
    if (!secant(c, g, root)) break;
    if (root < from) continue;
    if (root > end) break;
    out = static_cast<int64_t>(ceil(root - 0.5));
    if (out < fromUtcS) out = fromUtcS;
    return true;
  }
  return scan(c, from, end, out);
}

}  // namespace astro
}  // namespace ESPRack
