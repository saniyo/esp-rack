// History core (docs/plans/history.md): definitions, record layout and codec,
// the segment format and torn tails, put ordering, aggregation, downsampling,
// retention and segment names.
#include <unity.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include <history/HistCore.h>

using namespace ESPRack;
using namespace ESPRack::hist;

void setUp(void) {}
void tearDown(void) {}

// A wide series: two Sum columns, one value per channel.
static const HistColumn kWideCols[] = {
    {"in_mah", "mAh", HistAgg::Sum, 96, HistType::I16, 10},
    {"out_mah", "mAh", HistAgg::Sum, 96, HistType::I16, 10},
};
static const HistTier kWideTiers[] = {{3600, 7 * 86400}, {86400, 180 * 86400}};
static const HistSeriesDef kWide{"wide.p1", "Wide", kWideCols, 2, kWideTiers, 2};

// One of each aggregate, scalars.
static const HistColumn kAggCols[] = {
    {"s", "", HistAgg::Sum, 1, HistType::F32, 1},  {"a", "", HistAgg::Avg, 1, HistType::F32, 1},
    {"mn", "", HistAgg::Min, 1, HistType::F32, 1}, {"mx", "", HistAgg::Max, 1, HistType::F32, 1},
    {"l", "", HistAgg::Last, 1, HistType::F32, 1},
};
static const HistTier kAggTiers[] = {{300, 86400}};
static const HistSeriesDef kAgg{"agg", "", kAggCols, 5, kAggTiers, 1};

// SES months: a put-series of exact integers.
static const HistColumn kMonthCols[] = {
    {"cnt_mwh", "mWh", HistAgg::Last, 1, HistType::I64, 1},
    {"adj_mwh", "mWh", HistAgg::Last, 1, HistType::I64, 1},
    {"flags", "", HistAgg::Last, 1, HistType::U8, 1},
};
static const HistTier kMonthTiers[] = {{0, 400u * 86400u * 10u}};
static const HistSeriesDef kMonths{"ses.months", "Months", kMonthCols, 3, kMonthTiers, 1};

static void test_validate(void) {
  const char* why = nullptr;
  TEST_ASSERT_TRUE(validate(kWide, &why));
  TEST_ASSERT_TRUE(validate(kMonths, &why));
  TEST_ASSERT_TRUE(isPutSeries(kMonths));
  TEST_ASSERT_FALSE(isPutSeries(kWide));

  HistSeriesDef d = kWide;
  d.id = "Bad Id";
  TEST_ASSERT_FALSE(validate(d, &why));
  TEST_ASSERT_NOT_NULL(why);

  const HistTier twoPut[] = {{0, 86400}, {86400, 86400}};
  d = kMonths;
  d.tiers = twoPut;
  d.nTiers = 2;
  TEST_ASSERT_FALSE(validate(d, &why));  // a put-series has one tier

  const HistTier notMultiple[] = {{3600, 86400}, {5000, 86400}};
  d = kWide;
  d.tiers = notMultiple;
  TEST_ASSERT_FALSE(validate(d, &why));

  const HistColumn noScale[] = {{"x", "", HistAgg::Sum, 1, HistType::I16, 0}};
  d = kWide;
  d.cols = noScale;
  d.nCols = 1;
  TEST_ASSERT_FALSE(validate(d, &why));

  const HistColumn dup[] = {{"x", "", HistAgg::Sum, 1, HistType::F32, 1}, {"x", "", HistAgg::Sum, 1, HistType::F32, 1}};
  d.cols = dup;
  d.nCols = 2;
  TEST_ASSERT_FALSE(validate(d, &why));
}

static void test_layout(void) {
  Layout l0, l1;
  TEST_ASSERT_TRUE(layoutOf(kWide, 0, l0));
  TEST_ASSERT_TRUE(layoutOf(kWide, 1, l1));
  TEST_ASSERT_EQUAL_UINT(192, l0.nValues);
  TEST_ASSERT_EQUAL_UINT(4 + 192 * 2 + 2, l0.recSize);  // I16 hours
  TEST_ASSERT_EQUAL_UINT(4 + 192 * 4 + 2, l1.recSize);  // Sum widened to I32 for days
  TEST_ASSERT_TRUE(l1.types[0] == HistType::I32);
  TEST_ASSERT_TRUE(l0.schemaHash != l1.schemaHash);
  TEST_ASSERT_FALSE(layoutOf(kWide, 2, l0));

  // A different width (another topology) is another schema.
  HistColumn wider[] = {kWideCols[0], kWideCols[1]};
  wider[0].width = wider[1].width = 120;
  HistSeriesDef d = kWide;
  d.cols = wider;
  Layout lw;
  TEST_ASSERT_TRUE(layoutOf(d, 0, lw));
  TEST_ASSERT_TRUE(lw.schemaHash != l0.schemaHash);
}

static void test_record_roundtrip(void) {
  Layout l;
  TEST_ASSERT_TRUE(layoutOf(kWide, 0, l));
  std::vector<double> v(192), back(192);
  for (int i = 0; i < 192; ++i) v[i] = i * 1.25;
  v[3] = NAN;     // a cell with no sample
  v[4] = 99999;   // clamped (I16 x10)
  v[5] = -12.34;  // rounded to 0.1
  std::vector<uint8_t> rec(l.recSize);
  encodeRecord(kWide, l, 1761264000, v.data(), rec.data());
  uint32_t t = 0;
  TEST_ASSERT_TRUE(decodeRecord(kWide, l, rec.data(), t, back.data()));
  TEST_ASSERT_EQUAL_UINT32(1761264000, t);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 2.5, back[2]);
  TEST_ASSERT_TRUE(isnan(back[3]));                     // unknown stays unknown
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 3276.7, back[4]);     // the sentinel is kept free
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, -12.3, back[5]);

  rec[10] ^= 0x40;  // a flipped bit
  TEST_ASSERT_FALSE(decodeRecord(kWide, l, rec.data(), t, back.data()));
}

static void test_put_series_exact(void) {
  Layout l;
  TEST_ASSERT_TRUE(layoutOf(kMonths, 0, l));
  TEST_ASSERT_EQUAL_UINT(8 + 8 + 1, l.valueBytes);
  uint8_t raw[17];
  const int64_t cnt = 123456789012345LL, adj = -42;
  memcpy(raw, &cnt, 8);
  memcpy(raw + 8, &adj, 8);
  raw[16] = 0xFF;  // a real flag value, not "unknown" in a put-series
  std::vector<uint8_t> rec(l.recSize);
  encodeRaw(l, 1767225600, raw, rec.data());
  TEST_ASSERT_EQUAL_INT(0, memcmp(rec.data() + 4, raw, 17));  // stored exactly
  double v[3];
  uint32_t t = 0;
  TEST_ASSERT_TRUE(decodeRecord(kMonths, l, rec.data(), t, v));
  TEST_ASSERT_DOUBLE_WITHIN(0.5, 123456789012345.0, v[0]);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, -42.0, v[1]);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 255.0, v[2]);

  TEST_ASSERT_TRUE(putCheck(0, 100) == HistPut::Ok);
  TEST_ASSERT_TRUE(putCheck(100, 101) == HistPut::Ok);
  TEST_ASSERT_TRUE(putCheck(100, 100) == HistPut::Duplicate);
  TEST_ASSERT_TRUE(putCheck(100, 99) == HistPut::Stale);
}

static void test_segment_and_torn_tail(void) {
  SegHeader h;
  h.tier = 1;
  h.recSize = 774;
  h.schemaHash = 0xA1B2C3D4;
  h.bucketS = 86400;
  h.startT = 1761264000;
  uint8_t hb[kSegHeaderLen];
  encodeHeader(h, hb);
  SegHeader r;
  TEST_ASSERT_TRUE(decodeHeader(hb, sizeof hb, r));
  TEST_ASSERT_EQUAL_UINT8(1, r.tier);
  TEST_ASSERT_EQUAL_UINT16(774, r.recSize);
  TEST_ASSERT_EQUAL_UINT32(0xA1B2C3D4, r.schemaHash);
  hb[0] ^= 1;
  TEST_ASSERT_FALSE(decodeHeader(hb, sizeof hb, r));

  // Three records, then a torn fourth one.
  Layout l;
  TEST_ASSERT_TRUE(layoutOf(kAgg, 0, l));
  std::vector<uint8_t> seg;
  const double v[5] = {1, 2, 3, 4, 5};
  for (int i = 0; i < 4; ++i) {
    std::vector<uint8_t> rec(l.recSize);
    encodeRecord(kAgg, l, 1000 + i * 300, v, rec.data());
    seg.insert(seg.end(), rec.begin(), rec.end());
  }
  TEST_ASSERT_EQUAL_UINT(4, validRecords(seg.data(), seg.size(), l.recSize));
  seg.resize(seg.size() - 3);  // power cut mid-write
  TEST_ASSERT_EQUAL_UINT(3, validRecords(seg.data(), seg.size(), l.recSize));
  seg[l.recSize + 5] ^= 0xFF;  // a bad second record: stop there
  TEST_ASSERT_EQUAL_UINT(1, validRecords(seg.data(), seg.size(), l.recSize));
}

static void test_bucket_aggregates(void) {
  double acc[5];
  uint32_t cnt[5];
  Bucket b;
  b.attach(acc, cnt, 5);
  b.reset(3600);
  TEST_ASSERT_TRUE(b.empty());
  const double s1[5] = {1, 10, 5, 5, 7};
  const double s2[5] = {2, 20, 3, 9, NAN};  // the last value unknown this time
  b.add(kAgg, s1);
  b.add(kAgg, s2);
  double out[5];
  b.finish(kAgg, out);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 3, out[0]);   // sum
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 15, out[1]);  // avg
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 3, out[2]);   // min
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 9, out[3]);   // max
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 7, out[4]);   // last known

  b.reset(7200);
  b.finish(kAgg, out);
  // No sample: unknown for every aggregate, a Sum of nothing too.
  TEST_ASSERT_TRUE(isnan(out[0]) && isnan(out[1]) && isnan(out[2]) && isnan(out[3]) && isnan(out[4]));
  const double unknown[5] = {NAN, NAN, NAN, NAN, NAN};
  b.add(kAgg, unknown);
  b.finish(kAgg, out);
  TEST_ASSERT_TRUE(isnan(out[0]));  // only unknown samples: still unknown
  TEST_ASSERT_EQUAL_UINT32(7200, bucketStart(7200 + 3599, 3600));
}

struct Rows {
  std::vector<uint32_t> t;
  std::vector<double> v;
};
static void collect(void* ctx, uint32_t t, const double* v, uint16_t n) {
  Rows* r = static_cast<Rows*>(ctx);
  r->t.push_back(t);
  r->v.insert(r->v.end(), v, v + n);
}

static void test_downsample(void) {
  double acc[5], out[5];
  uint32_t cnt[5];
  Rows rows;
  Downsampler ds;
  ds.attach(kAgg, 3600, acc, cnt, out, 5, collect, &rows);
  // 5-minute rows over two hours.
  for (int i = 0; i < 24; ++i) {
    const double v[5] = {1, double(i), double(i), double(i), double(i)};
    ds.feed(uint32_t(i * 300), v);
  }
  ds.finish();
  TEST_ASSERT_EQUAL_UINT(2, rows.t.size());
  TEST_ASSERT_EQUAL_UINT32(0, rows.t[0]);
  TEST_ASSERT_EQUAL_UINT32(3600, rows.t[1]);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 12, rows.v[0]);    // sum of 12 ones
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 5.5, rows.v[1]);   // avg 0..11
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 12, rows.v[5 + 2]);  // min of the second hour
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 23, rows.v[5 + 4]);  // last

  // res 0: the whole period in one row (e.g. totals per cell).
  Rows all;
  ds.attach(kAgg, 0, acc, cnt, out, 5, collect, &all);
  for (int i = 0; i < 24; ++i) {
    const double v[5] = {2, 1, double(i), double(i), double(i)};
    ds.feed(uint32_t(1000 + i * 300), v);
  }
  ds.finish();
  TEST_ASSERT_EQUAL_UINT(1, all.t.size());
  TEST_ASSERT_EQUAL_UINT32(1000, all.t[0]);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 48, all.v[0]);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0, all.v[2]);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 23, all.v[3]);
}

static void test_retention(void) {
  const SegInfo segs[] = {{1000, 500}, {2000, 500}, {3000, 500}, {4000, 500}};
  bool drop[4];
  retention(segs, 4, 4100, 1500, 0, drop);  // keep 1500 s: anything newest < 2600 goes
  TEST_ASSERT_TRUE(drop[0] && drop[1] && !drop[2] && !drop[3]);
  retention(segs, 4, 4100, 100000, 1200, drop);  // size cap: keep the newest ones that fit
  TEST_ASSERT_TRUE(drop[0] && drop[1] && !drop[2] && !drop[3]);
  retention(segs, 4, 999999, 1, 1, drop);  // the newest segment is never dropped
  TEST_ASSERT_TRUE(drop[0] && drop[1] && drop[2] && !drop[3]);
}

static void test_names_and_columns(void) {
  TEST_ASSERT_EQUAL_UINT32(19700101, segmentKey(0, 3600));
  TEST_ASSERT_EQUAL_UINT32(20000229, segmentKey(951782400, 300));
  TEST_ASSERT_EQUAL_UINT32(20251024, segmentKey(1761350399, 3600));
  TEST_ASSERT_EQUAL_UINT32(202510, segmentKey(1761264000, 86400));
  TEST_ASSERT_EQUAL_UINT32(202601, segmentKey(1767225600, 0));

  uint8_t col = 0;
  int el = 0;
  TEST_ASSERT_TRUE(findColumn(kWide, "out_mah", 7, col, el));
  TEST_ASSERT_EQUAL_UINT8(1, col);
  TEST_ASSERT_EQUAL_INT(-1, el);
  TEST_ASSERT_TRUE(findColumn(kWide, "in_mah[95]", 10, col, el));
  TEST_ASSERT_EQUAL_INT(95, el);
  TEST_ASSERT_FALSE(findColumn(kWide, "in_mah[96]", 10, col, el));
  TEST_ASSERT_FALSE(findColumn(kWide, "nope", 4, col, el));
  TEST_ASSERT_FALSE(findColumn(kWide, "in_mah[x]", 9, col, el));
  TEST_ASSERT_TRUE(findColumn(kWide, "in_mah[min]", 11, col, el));
  TEST_ASSERT_EQUAL_INT(kRowMin, el);
  TEST_ASSERT_TRUE(findColumn(kWide, "out_mah[avg]", 12, col, el));
  TEST_ASSERT_EQUAL_INT(kRowAvg, el);
  TEST_ASSERT_TRUE(findColumn(kWide, "out_mah[max]", 12, col, el));
  TEST_ASSERT_EQUAL_INT(kRowMax, el);
  const double row[4] = {3300, NAN, 3290, 3310};
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 3290, rowDerived(row, 4, kRowMin));
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 3300, rowDerived(row, 4, kRowAvg));  // the unknown cell doesn't count
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 3310, rowDerived(row, 4, kRowMax));
  const double none[2] = {NAN, NAN};
  TEST_ASSERT_TRUE(isnan(rowDerived(none, 2, kRowAvg)));

  TEST_ASSERT_EQUAL_STRING("sum", histAggName(HistAgg::Sum));
  TEST_ASSERT_EQUAL_STRING("i64", histTypeName(HistType::I64));
  TEST_ASSERT_EQUAL_STRING("full", histPutName(HistPut::Full));
}

// Local days: a fixed UTC+2 zone, and one that falls back from +3 to +2 at
// 01:00 UTC on day D.
static const uint32_t D = 1761264000;  // a UTC midnight
static int32_t plus2(uint32_t) { return 7200; }
static int32_t fallBack(uint32_t t) { return t < D + 3600 ? 10800 : 7200; }

static void test_local_days(void) {
  const HistTier tiers[] = {{3600, 7 * 86400}, {86400, 365 * 86400}};
  HistSeriesDef utc{"days", "Days", kAggCols, 5, tiers, 2};
  HistSeriesDef local = utc;
  local.localDays = true;
  TEST_ASSERT_FALSE(localTier(local, 0));
  TEST_ASSERT_TRUE(localTier(local, 1));
  TEST_ASSERT_FALSE(localTier(utc, 1));

  // UTC tiers and hour tiers: plain bucketStart.
  TEST_ASSERT_EQUAL_UINT32(D, tierStart(utc, 1, D + 43200, &plus2));
  TEST_ASSERT_EQUAL_UINT32(D + 39600 - 39600 % 3600, tierStart(local, 0, D + 39600 + 5, &plus2));
  TEST_ASSERT_EQUAL_UINT32(D, tierStart(local, 1, D + 43200, nullptr));

  // UTC+2: the local day starts at 22:00 UTC the day before.
  TEST_ASSERT_EQUAL_UINT32(D - 7200, tierStart(local, 1, D + 43200, &plus2));
  TEST_ASSERT_EQUAL_UINT32(D + 86400 - 7200, tierStart(local, 1, D + 82800, &plus2));
  TEST_ASSERT_EQUAL_UINT32(D - 7200, tierStart(local, 1, D - 7200, &plus2));

  // Fall back: the day starting at D - 3 h (summer midnight) lasts 25 h.
  const uint32_t day = tierStart(local, 1, D + 43200, &fallBack);
  const uint32_t next = tierStart(local, 1, D + 86400 + 3600, &fallBack);
  TEST_ASSERT_EQUAL_UINT32(D - 10800, day);
  TEST_ASSERT_EQUAL_UINT32(90000, next - day);
  TEST_ASSERT_EQUAL_UINT32(D - 86400 - 10800, tierStart(local, 1, D - 43200, &fallBack));

  // Part of the schema: other bucket starts, other files.
  Layout a, b;
  TEST_ASSERT_TRUE(layoutOf(utc, 1, a) && layoutOf(local, 1, b));
  TEST_ASSERT_TRUE(a.schemaHash != b.schemaHash);
  TEST_ASSERT_TRUE(layoutOf(utc, 0, a) && layoutOf(local, 0, b));
  TEST_ASSERT_EQUAL_UINT32(a.schemaHash, b.schemaHash);
}

static void test_split_by_need(void) {
  uint32_t share[3];
  const uint64_t need[] = {100, 300};
  splitByNeed(1000, need, 2, share);
  TEST_ASSERT_EQUAL_UINT32(250, share[0]);
  TEST_ASSERT_EQUAL_UINT32(750, share[1]);

  const uint64_t none[] = {0, 0, 0};  // unknown needs: equal shares
  splitByNeed(30, none, 3, share);
  TEST_ASSERT_EQUAL_UINT32(10, share[0]);
  TEST_ASSERT_EQUAL_UINT32(10, share[2]);

  const uint64_t lopsided[] = {1, 1000000};  // never 0: that would mean no cap
  splitByNeed(1, lopsided, 2, share);
  TEST_ASSERT_EQUAL_UINT32(1, share[0]);
  TEST_ASSERT_EQUAL_UINT32(1, share[1]);

  Layout l;
  TEST_ASSERT_TRUE(layoutOf(kWide, 0, l));
  TEST_ASSERT_EQUAL_UINT64(uint64_t(7 * 24 + 1) * l.recSize, tierNeed(kWide, 0, l.recSize));
  const HistTier putTier[] = {{0, 365 * 86400}};
  HistSeriesDef put{"rec", "Records", kAggCols, 5, putTier, 1};
  TEST_ASSERT_EQUAL_UINT64(0, tierNeed(put, 0, 64));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_validate);
  RUN_TEST(test_layout);
  RUN_TEST(test_record_roundtrip);
  RUN_TEST(test_put_series_exact);
  RUN_TEST(test_segment_and_torn_tail);
  RUN_TEST(test_bucket_aggregates);
  RUN_TEST(test_downsample);
  RUN_TEST(test_retention);
  RUN_TEST(test_names_and_columns);
  RUN_TEST(test_local_days);
  RUN_TEST(test_split_by_need);
  return UNITY_END();
}
