#!/usr/bin/env python3
"""Generate test-host/fixtures/sun_fixture.h for the SunCalc host suite.

Model: the NOAA Solar Calculator equations (the spreadsheet's columns, in
Python). Before writing, every position is cross-checked against an
independent model - the USNO/Astronomical Almanac low-precision solar
coordinates with GMST - so a transcription slip in the NOAA terms cannot leak
into both the oracle and the C++ code.

Contents (grid 47..52 N x 22..40 E, solstices and equinoxes of 2026 and 2030):
  - positions: geometric elevation, refracted elevation, azimuth;
  - crossings: the exact first crossing (60 s scan + bisection to 1 ms) of the
    geometric elevation through h0 in {-0.833, -6, 0} deg, both directions,
    from 00:00 and 12:00 UTC, 36 h horizon; plus 70 N / 78 N rows where the
    answer can be "none" (polar day / night), and 70 N rows around the edges of
    the polar day and night, where the hour-angle guess does not apply and
    nextCrossing() falls back to its 10-minute scan. Edge rows whose sun stays on
    either side of h0 for less than 20 min around the crossing are left out:
    the 10-minute scan may legitimately step over such a short dip.

    python scripts/gen_sun_fixture.py           # (re)write the header
    python scripts/gen_sun_fixture.py --check   # exit 1 when the header is stale
"""
import argparse
import calendar
import math
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "test-host" / "fixtures" / "sun_fixture.h"

LATS = [47.0, 49.5, 52.0]
LONS = [22.0, 31.0, 40.0]
POLAR_LATS = [70.0, 78.0]
POLAR_LON = 25.0
DATES = [(2026, 3, 20), (2026, 6, 21), (2026, 9, 23), (2026, 12, 21),
         (2030, 3, 20), (2030, 6, 21), (2030, 9, 22), (2030, 12, 21)]
POS_HOURS = [h * 3600 + 17 * 60 for h in range(0, 24, 3)]  # 00:17, 03:17, ...
H0S = [-0.833, -6.0, 0.0]
HORIZON = 129600
EDGE_LAT, EDGE_LON = 70.0, 25.0
EDGE_DAYS = [(2026, 5, 14), (2026, 7, 24), (2026, 11, 22), (2027, 1, 14)]  # 9 days from each
ROBUST_S = 1200
CROSSCHECK_ELEV = 0.05  # deg, NOAA vs USNO
CROSSCHECK_AZ = 0.10    # deg, when the sun is not near the zenith


def utc(y, m, d, s=0):
    return calendar.timegm((y, m, d, 0, 0, 0)) + s


def noaa(t):
    """-> (declination rad, equation of time min)"""
    jd = t / 86400.0 + 2440587.5
    T = (jd - 2451545.0) / 36525.0
    L0 = (280.46646 + T * (36000.76983 + T * 0.0003032)) % 360.0
    M = math.radians(357.52911 + T * (35999.05029 - 0.0001537 * T))
    e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T)
    C = (math.sin(M) * (1.914602 - T * (0.004817 + 0.000014 * T)) + math.sin(2 * M) * (0.019993 - 0.000101 * T)
         + math.sin(3 * M) * 0.000289)
    omega = math.radians(125.04 - 1934.136 * T)
    app = math.radians(L0 + C - 0.00569 - 0.00478 * math.sin(omega))
    eps0 = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0
    eps = math.radians(eps0 + 0.00256 * math.cos(omega))
    y = math.tan(eps / 2) ** 2
    L0r = math.radians(L0)
    eqt = 4.0 * math.degrees(y * math.sin(2 * L0r) - 2 * e * math.sin(M) + 4 * e * y * math.sin(M) * math.cos(2 * L0r)
                             - 0.5 * y * y * math.sin(4 * L0r) - 1.25 * e * e * math.sin(2 * M))
    return math.asin(math.sin(eps) * math.sin(app)), eqt


def noaa_pos(t, lat, lon):
    decl, eqt = noaa(t)
    tst = ((t % 86400) / 60.0 + eqt + 4.0 * lon) % 1440.0
    ha = tst / 4.0 - 180.0
    latr, har = math.radians(lat), math.radians(ha)
    cz = max(-1.0, min(1.0, math.sin(latr) * math.sin(decl) + math.cos(latr) * math.cos(decl) * math.cos(har)))
    zen = math.degrees(math.acos(cz))
    # NOAA spreadsheet azimuth form (acos), as opposed to the C++ atan2 form
    den = math.cos(latr) * math.sin(math.radians(zen))
    a = math.degrees(math.acos(max(-1.0, min(1.0, (math.sin(latr) * math.cos(math.radians(zen)) - math.sin(decl)) / den))))
    az = (a + 180.0) % 360.0 if ha > 0 else (540.0 - a) % 360.0
    return 90.0 - zen, az


def refraction(e):
    if e > 85.0:
        return 0.0
    te = math.tan(math.radians(e))
    if e > 5.0:
        s = 58.1 / te - 0.07 / te ** 3 + 0.000086 / te ** 5
    elif e > -0.575:
        s = 1735.0 + e * (-518.2 + e * (103.4 + e * (-12.79 + e * 0.711)))
    else:
        s = -20.772 / te
    return s / 3600.0


def usno_pos(t, lat, lon):
    """USNO 'approximate solar coordinates' (~1 arcmin, 1950..2050)."""
    D = t / 86400.0 + 2440587.5 - 2451545.0
    g = math.radians((357.529 + 0.98560028 * D) % 360)
    q = (280.459 + 0.98564736 * D) % 360
    L = math.radians(q + 1.915 * math.sin(g) + 0.020 * math.sin(2 * g))
    e = math.radians(23.439 - 0.00000036 * D)
    ra = math.atan2(math.cos(e) * math.sin(L), math.cos(L))
    dec = math.asin(math.sin(e) * math.sin(L))
    gmst = (18.697374558 + 24.06570982441908 * D) % 24
    lha = math.radians(gmst * 15 + lon) - ra
    latr = math.radians(lat)
    elev = math.degrees(math.asin(math.sin(latr) * math.sin(dec) + math.cos(latr) * math.cos(dec) * math.cos(lha)))
    az = (math.degrees(math.atan2(math.sin(lha), math.cos(lha) * math.sin(latr) - math.tan(dec) * math.cos(latr)))
          + 180.0) % 360.0
    return elev, az


def crossing(t0, lat, lon, h0, up):
    f = lambda t: noaa_pos(t, lat, lon)[0] - h0
    hit = (lambda a, b: a < 0 <= b) if up else (lambda a, b: a >= 0 > b)
    tp, fp = t0, f(t0)
    end = t0 + HORIZON
    while tp < end:
        t = min(tp + 60, end)
        fc = f(t)
        if hit(fp, fc):
            lo, hi = tp, t
            while hi - lo > 1e-3:
                mid = (lo + hi) / 2
                if hit(fp, f(mid)):
                    hi = mid
                else:
                    lo = mid
            return round(hi)
        tp, fp = t, fc
    return -1


def robust(r, t0, lat, lon, h0, up):
    """The sun stays below/above h0 for ROBUST_S before (or since t0) and after r."""
    f = lambda t: noaa_pos(t, lat, lon)[0] - h0
    for k in range(1, ROBUST_S // 30 + 1):
        tb, ta = r - 30 * k, r + 30 * k
        if tb >= t0 and (f(tb) >= 0) == up:
            return False
        if (f(ta) >= 0) != up:
            return False
    return True


def crosscheck(t, lat, lon, elev, az):
    ue, ua = usno_pos(t, lat, lon)
    if abs(ue - elev) > CROSSCHECK_ELEV:
        raise SystemExit(f"NOAA/USNO elevation mismatch t={t} lat={lat} lon={lon}: {elev:.4f} vs {ue:.4f}")
    if elev < 80 and abs((ua - az + 180) % 360 - 180) > CROSSCHECK_AZ:
        raise SystemExit(f"NOAA/USNO azimuth mismatch t={t} lat={lat} lon={lon}: {az:.4f} vs {ua:.4f}")


def generate():
    lines = [
        "// Generated by scripts/gen_sun_fixture.py - do not edit.",
        "// Model: NOAA Solar Calculator equations, cross-checked against the USNO",
        f"// low-precision solar coordinates (elevation +-{CROSSCHECK_ELEV} deg, azimuth +-{CROSSCHECK_AZ} deg).",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "namespace sunfx {",
        "",
        "constexpr double kLat[] = {" + ", ".join(f"{v}" for v in LATS + POLAR_LATS) + "};",
        "constexpr double kLon[] = {" + ", ".join(f"{v}" for v in LONS + [POLAR_LON]) + "};",
        f"constexpr int32_t kHorizonS = {HORIZON};",
        "",
        "struct Pos { int64_t utc; uint8_t lat, lon; double elev, elevRefr, az; };",
        "constexpr Pos kPos[] = {",
    ]
    npos = 0
    for (y, m, d) in DATES:
        for li, lat in enumerate(LATS):
            for oi, lon in enumerate(LONS):
                for s in POS_HOURS:
                    t = utc(y, m, d, s)
                    elev, az = noaa_pos(t, lat, lon)
                    crosscheck(t, lat, lon, elev, az)
                    lines.append(f"    {{{t}LL, {li}, {oi}, {elev:.5f}, {elev + refraction(elev):.5f}, {az:.5f}}},")
                    npos += 1
    lines += [
        "};",
        f"constexpr int kPosCount = {npos};",
        "",
        "// expect = -1: no crossing within kHorizonS",
        "struct Cross { int64_t from; uint8_t lat, lon; double h0; bool up; int64_t expect; };",
        "constexpr Cross kCross[] = {",
    ]
    ncross = 0
    for (y, m, d) in DATES:
        for li, lat in enumerate(LATS):
            for oi, lon in enumerate(LONS):
                for fs in (0, 43200):
                    t0 = utc(y, m, d, fs)
                    for h0 in H0S:
                        for up in (True, False):
                            r = crossing(t0, lat, lon, h0, up)
                            lines.append(f"    {{{t0}LL, {li}, {oi}, {h0}, {'true' if up else 'false'}, {r}LL}},")
                            ncross += 1
        for pi, lat in enumerate(POLAR_LATS):
            t0 = utc(y, m, d)
            for up in (True, False):
                r = crossing(t0, lat, POLAR_LON, -0.833, up)
                lines.append(f"    {{{t0}LL, {len(LATS) + pi}, {len(LONS)}, -0.833, {'true' if up else 'false'}, {r}LL}},")
                ncross += 1
    skipped = 0
    for (y, m, d) in EDGE_DAYS:
        for k in range(9):
            for fs in (0, 43200):
                t0 = utc(y, m, d, fs) + k * 86400
                for up in (True, False):
                    r = crossing(t0, EDGE_LAT, EDGE_LON, -0.833, up)
                    if r >= 0 and not robust(r, t0, EDGE_LAT, EDGE_LON, -0.833, up):
                        skipped += 1
                        continue
                    lines.append(f"    {{{t0}LL, {len(LATS)}, {len(LONS)}, -0.833, {'true' if up else 'false'}, {r}LL}},")
                    ncross += 1
    lines += [
        "};",
        f"constexpr int kCrossCount = {ncross};",
        f"// polar-edge rows left out (dip shorter than {ROBUST_S // 60} min): {skipped}",
        "",
        "}  // namespace sunfx",
        "",
    ]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if the header differs")
    args = ap.parse_args()
    text = generate()
    if args.check:
        cur = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if cur != text:
            print(f"{OUT.relative_to(ROOT)} is stale - run scripts/gen_sun_fixture.py")
            return 1
        print("sun fixture up to date")
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
