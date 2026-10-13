// SES-LIB-006 acceptance 3: sunPosition ±0.1°, crossings ±60 s vs the NOAA
// model (scripts/gen_sun_fixture.py, cross-checked against USNO) for
// 47–52°N / 22–40°E at solstices and equinoxes; polar -> false, no hang.
#include <math.h>
#include <stdio.h>
#include <unity.h>

#include <ESPRackPure/Civil.h>
#include <ESPRackPure/SunCalc.h>
#include <sun_fixture.h>

using namespace ESPRack;
using astro::Dir;

void setUp(void) {}
void tearDown(void) {}

static double angleDiff(double a, double b) {
  double d = fmod(a - b + 540.0, 360.0) - 180.0;
  return fabs(d);
}

static void test_positions(void) {
  double maxE = 0, maxR = 0, maxA = 0;
  for (int i = 0; i < sunfx::kPosCount; ++i) {
    const sunfx::Pos& p = sunfx::kPos[i];
    const double lat = sunfx::kLat[p.lat], lon = sunfx::kLon[p.lon];
    const astro::SunPos g = astro::sunPosition(p.utc, lat, lon);
    const astro::SunPos r = astro::sunPosition(p.utc, lat, lon, true);
    maxE = fmax(maxE, fabs(g.elevDeg - p.elev));
    maxR = fmax(maxR, fabs(r.elevDeg - p.elevRefr));
    maxA = fmax(maxA, angleDiff(g.azDeg, p.az));
    TEST_ASSERT_TRUE(g.azDeg >= 0.0 && g.azDeg < 360.0);
  }
  char msg[120];
  snprintf(msg, sizeof msg, "max |dElev| %.6f, |dElevRefr| %.6f, |dAz| %.6f deg", maxE, maxR, maxA);
  TEST_MESSAGE(msg);
  TEST_ASSERT_TRUE_MESSAGE(maxE <= 0.1 && maxR <= 0.1 && maxA <= 0.1, msg);  // acceptance
  TEST_ASSERT_TRUE_MESSAGE(maxE <= 1e-3 && maxR <= 1e-3 && maxA <= 1e-3, msg);  // same model: rounding only
}

static void test_crossings(void) {
  int64_t maxErr = 0;
  int bad = 0, none = 0;
  char msg[200] = {0};
  for (int i = 0; i < sunfx::kCrossCount; ++i) {
    const sunfx::Cross& c = sunfx::kCross[i];
    int64_t out = 0;
    const bool found = astro::nextCrossing(c.from, sunfx::kLat[c.lat], sunfx::kLon[c.lon], c.h0,
                                           c.up ? Dir::Ascending : Dir::Descending, out, sunfx::kHorizonS);
    bool good;
    if (c.expect < 0) {
      good = !found;
      ++none;
    } else {
      const int64_t err = found ? (out > c.expect ? out - c.expect : c.expect - out) : INT64_MAX;
      if (found && err > maxErr) maxErr = err;
      good = found && err <= 60 && out >= c.from;
    }
    if (!good && bad++ == 0)
      snprintf(msg, sizeof msg, "row %d from=%lld lat=%.1f lon=%.1f h0=%.3f %s: found=%d out=%lld expect=%lld", i,
               (long long)c.from, sunfx::kLat[c.lat], sunfx::kLon[c.lon], c.h0, c.up ? "asc" : "desc", (int)found,
               (long long)out, (long long)c.expect);
  }
  if (bad) TEST_FAIL_MESSAGE(msg);
  snprintf(msg, sizeof msg, "max crossing error %lld s over %d rows (%d polar 'none')", (long long)maxErr,
           sunfx::kCrossCount, none);
  TEST_MESSAGE(msg);
  TEST_ASSERT_TRUE(none > 0);
  TEST_ASSERT_TRUE_MESSAGE(maxErr <= 2, msg);  // same model: the root finder is the only difference
}

static void test_solar_noon(void) {
  static const int32_t dates[][3] = {{2026, 3, 20}, {2026, 6, 21}, {2026, 11, 3}, {2026, 12, 21}, {2030, 2, 11}};
  static const double lons[] = {22.0, 31.0, 40.0, -75.0, 151.0};
  for (const auto& d : dates) {
    const int64_t day = civil::utcFromCivil(d[0], (uint32_t)d[1], (uint32_t)d[2], 0, 0, 0);
    for (double lon : lons) {
      const int64_t meanNoon = day + 43200 - (int64_t)(lon * 240.0);
      const int64_t noon = astro::solarNoon(meanNoon, lon);
      TEST_ASSERT_TRUE(noon > meanNoon - 17 * 60 && noon < meanNoon + 17 * 60);  // |EoT| < 17 min
      // the same noon for any instant of that local mean solar day
      TEST_ASSERT_EQUAL_INT64(noon, astro::solarNoon(meanNoon - 43200 + 1, lon));
      TEST_ASSERT_EQUAL_INT64(noon, astro::solarNoon(meanNoon + 43199, lon));
      // ... and the next day's noon after it (EoT drifts < 30 s/day)
      TEST_ASSERT_INT64_WITHIN(30, noon + 86400, astro::solarNoon(meanNoon + 43200 + 1, lon));
      // culmination: elevation at noon is the maximum, azimuth is south (north of the tropic)
      const double e = astro::sunPosition(noon, 50.0, lon).elevDeg;
      TEST_ASSERT_TRUE(e >= astro::sunPosition(noon - 120, 50.0, lon).elevDeg);
      TEST_ASSERT_TRUE(e >= astro::sunPosition(noon + 120, 50.0, lon).elevDeg);
      TEST_ASSERT_TRUE(angleDiff(astro::sunPosition(noon, 50.0, lon).azDeg, 180.0) < 0.05);
    }
  }
}

static void test_polar_and_horizon(void) {
  const int64_t jun = civil::utcFromCivil(2026, 6, 21, 0, 0, 0);
  const int64_t dec = civil::utcFromCivil(2026, 12, 21, 0, 0, 0);
  const int64_t mar = civil::utcFromCivil(2026, 3, 20, 0, 0, 0);
  int64_t out = 0;
  // midnight sun / polar night: no sunset / sunrise within 36 h
  TEST_ASSERT_FALSE(astro::nextCrossing(jun, 75.0, 25.0, -0.833, Dir::Descending, out));
  TEST_ASSERT_FALSE(astro::nextCrossing(dec, 75.0, 25.0, -0.833, Dir::Ascending, out));
  TEST_ASSERT_FALSE(astro::nextCrossing(jun, -75.0, 25.0, -0.833, Dir::Ascending, out));
  // the poles themselves (cos(lat) = 0) and near them: terminate, sensible answers
  TEST_ASSERT_FALSE(astro::nextCrossing(jun, 90.0, 0.0, -0.833, Dir::Descending, out));
  TEST_ASSERT_FALSE(astro::nextCrossing(jun, -90.0, 0.0, -0.833, Dir::Ascending, out));
  TEST_ASSERT_FALSE(astro::nextCrossing(dec, 89.9, 0.0, -0.833, Dir::Ascending, out));
  for (int i = 0; i < 48; ++i) {  // equinox at the pole: grazing, must not hang
    const double lat = (i % 2) ? 90.0 : 89.0;
    astro::nextCrossing(mar + i * 3600, lat, i * 7.5, -0.833, (i % 4) < 2 ? Dir::Ascending : Dir::Descending, out);
  }
  // horizon limits
  TEST_ASSERT_FALSE(astro::nextCrossing(mar, 50.0, 30.0, -0.833, Dir::Ascending, out, 600));
  TEST_ASSERT_FALSE(astro::nextCrossing(mar, 50.0, 30.0, -0.833, Dir::Ascending, out, 0));
  TEST_ASSERT_TRUE(astro::nextCrossing(mar, 50.0, 30.0, -0.833, Dir::Ascending, out, 6 * 3600));
  int64_t a = 0, b = 0;
  // a 10 h horizon from 01:00 still finds sunrise, through the scan if needed
  TEST_ASSERT_TRUE(astro::nextCrossing(mar + 3600, 50.0, 30.0, -0.833, Dir::Ascending, a, 10 * 3600));
  TEST_ASSERT_TRUE(astro::nextCrossing(mar + 3600, 50.0, 30.0, -0.833, Dir::Ascending, b));
  TEST_ASSERT_EQUAL_INT64(b, a);
  // the next crossing is strictly after one that just happened
  TEST_ASSERT_TRUE(astro::nextCrossing(b + 1, 50.0, 30.0, -0.833, Dir::Ascending, a));
  TEST_ASSERT_TRUE(a > b + 86400 - 600 && a < b + 86400 + 600);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_positions);
  RUN_TEST(test_crossings);
  RUN_TEST(test_solar_noon);
  RUN_TEST(test_polar_and_horizon);
  return UNITY_END();
}
