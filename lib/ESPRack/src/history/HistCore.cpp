#include <history/HistCore.h>

#include <math.h>
#include <string.h>

namespace ESPRack {

const char* histAggName(HistAgg a) {
  switch (a) {
    case HistAgg::Sum: return "sum";
    case HistAgg::Avg: return "avg";
    case HistAgg::Min: return "min";
    case HistAgg::Max: return "max";
    case HistAgg::Last: return "last";
  }
  return "?";
}

const char* histTypeName(HistType t) {
  switch (t) {
    case HistType::I8: return "i8";
    case HistType::U8: return "u8";
    case HistType::I16: return "i16";
    case HistType::U16: return "u16";
    case HistType::I32: return "i32";
    case HistType::U32: return "u32";
    case HistType::I64: return "i64";
    case HistType::U64: return "u64";
    case HistType::F32: return "f32";
  }
  return "?";
}

const char* histPutName(HistPut p) {
  switch (p) {
    case HistPut::Ok: return "ok";
    case HistPut::Duplicate: return "duplicate";
    case HistPut::Stale: return "stale";
    case HistPut::Full: return "full";
    case HistPut::NotReady: return "not-ready";
    case HistPut::BadArgs: return "bad-args";
  }
  return "?";
}

size_t histTypeSize(HistType t) {
  switch (t) {
    case HistType::I8:
    case HistType::U8: return 1;
    case HistType::I16:
    case HistType::U16: return 2;
    case HistType::I32:
    case HistType::U32:
    case HistType::F32: return 4;
    case HistType::I64:
    case HistType::U64: return 8;
  }
  return 0;
}

namespace hist {

namespace {

void putU16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}
void putU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
void putU64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
}
uint16_t getU16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t getU32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t getU64(const uint8_t* p) { return uint64_t(getU32(p)) | (uint64_t(getU32(p + 4)) << 32); }

// FNV-1a, 32 bits.
void fnv(uint32_t& h, const void* d, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(d);
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 16777619u;
  }
}

bool isLdhDots(const char* s) {
  if (!s || !*s) return false;
  size_t n = 0;
  for (; s[n]; ++n) {
    const char c = s[n];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
  }
  return n <= kMaxIdLen;
}

// value * scale, rounded half away from zero, clamped to [lo, hi].
double scaled(double v, float scale, double lo, double hi) {
  double s = v * double(scale);
  s = s >= 0 ? floor(s + 0.5) : ceil(s - 0.5);
  if (s < lo) s = lo;
  if (s > hi) s = hi;
  return s;
}

// Aggregated (bucketed) series mark an element that got no sample with the
// type's sentinel: the lowest signed / highest unsigned value, NaN for f32.
// Values are clamped short of it. Put-series store exactly what they're given.
void putValue(uint8_t* p, HistType t, float scale, double v, bool sentinel) {
  const bool none = isnan(v);
  if (none && !sentinel) v = 0;
  const double sl = sentinel ? 1 : 0;  // keep the sentinel free
  switch (t) {
    case HistType::I8: p[0] = uint8_t(none && sentinel ? int8_t(-128) : int8_t(scaled(v, scale, -128 + sl, 127))); break;
    case HistType::U8: p[0] = none && sentinel ? 0xFF : uint8_t(scaled(v, scale, 0, 255 - sl)); break;
    case HistType::I16:
      putU16(p, uint16_t(none && sentinel ? int16_t(-32768) : int16_t(scaled(v, scale, -32768 + sl, 32767))));
      break;
    case HistType::U16: putU16(p, none && sentinel ? 0xFFFF : uint16_t(scaled(v, scale, 0, 65535 - sl))); break;
    case HistType::I32:
      putU32(p, none && sentinel ? 0x80000000u : uint32_t(int32_t(scaled(v, scale, -2147483648.0 + sl, 2147483647.0))));
      break;
    case HistType::U32: putU32(p, none && sentinel ? 0xFFFFFFFFu : uint32_t(scaled(v, scale, 0, 4294967295.0 - sl))); break;
    case HistType::I64:
      putU64(p, none && sentinel ? 0x8000000000000000ull
                                 : uint64_t(int64_t(scaled(v, scale, -9.2233720368547748e18 + 2048 * sl,
                                                           9.2233720368547748e18 - 2048))));
      break;
    case HistType::U64:
      putU64(p, none && sentinel ? 0xFFFFFFFFFFFFFFFFull
                                 : uint64_t(scaled(v, scale, 0, 1.8446744073709550e19 - 4096 * (1 + sl))));
      break;
    case HistType::F32: {
      const float f = float(v);  // NaN stays NaN
      uint32_t bits;
      memcpy(&bits, &f, 4);
      putU32(p, bits);
      break;
    }
  }
}

double getValue(const uint8_t* p, HistType t, float scale, bool sentinel) {
  const double s = scale == 0 ? 1.0 : double(scale);
  switch (t) {
    case HistType::I8: return sentinel && p[0] == 0x80 ? NAN : int8_t(p[0]) / s;
    case HistType::U8: return sentinel && p[0] == 0xFF ? NAN : p[0] / s;
    case HistType::I16: return sentinel && getU16(p) == 0x8000 ? NAN : int16_t(getU16(p)) / s;
    case HistType::U16: return sentinel && getU16(p) == 0xFFFF ? NAN : getU16(p) / s;
    case HistType::I32: return sentinel && getU32(p) == 0x80000000u ? NAN : int32_t(getU32(p)) / s;
    case HistType::U32: return sentinel && getU32(p) == 0xFFFFFFFFu ? NAN : getU32(p) / s;
    case HistType::I64: return sentinel && getU64(p) == 0x8000000000000000ull ? NAN : double(int64_t(getU64(p))) / s;
    case HistType::U64: return sentinel && getU64(p) == 0xFFFFFFFFFFFFFFFFull ? NAN : double(getU64(p)) / s;
    case HistType::F32: {
      const uint32_t bits = getU32(p);
      float f;
      memcpy(&f, &bits, 4);
      return f;
    }
  }
  return 0;
}

}  // namespace

bool isPutSeries(const HistSeriesDef& def) { return def.nTiers > 0 && def.tiers && def.tiers[0].bucketS == 0; }

bool validate(const HistSeriesDef& def, const char** why) {
  const char* w = nullptr;
  size_t values = 0;
  if (!isLdhDots(def.id)) {
    w = "id must be 1..31 of a-z 0-9 - _ .";
  } else if (!def.cols || def.nCols == 0 || def.nCols > kMaxCols) {
    w = "1..16 columns";
  } else if (!def.tiers || def.nTiers == 0 || def.nTiers > kMaxTiers) {
    w = "1..4 tiers";
  } else if (isPutSeries(def) && def.nTiers != 1) {
    w = "a put-series (bucketS 0) has one tier";
  }
  for (uint8_t i = 0; !w && i < def.nCols; ++i) {
    const HistColumn& c = def.cols[i];
    if (!isLdhDots(c.key) || strchr(c.key, '.')) w = "column key must be 1..31 of a-z 0-9 - _";
    else if (c.width == 0) w = "column width must be at least 1";
    else if (histTypeSize(c.type) == 0) w = "unknown column type";
    else if (c.type != HistType::F32 && !(c.scale > 0)) w = "integer columns need a positive scale";
    for (uint8_t j = 0; !w && j < i; ++j) {
      if (strcmp(def.cols[j].key, c.key) == 0) w = "duplicate column key";
    }
    values += c.width;
  }
  if (!w && values > kMaxValues) w = "more than 1024 values per record";
  for (uint8_t i = 1; !w && i < def.nTiers; ++i) {
    const uint32_t prev = def.tiers[i - 1].bucketS, cur = def.tiers[i].bucketS;
    if (cur <= prev || cur % prev) w = "each tier's bucket must be a multiple of the previous one";
  }
  for (uint8_t i = 0; !w && i < def.nTiers; ++i) {
    if (def.tiers[i].keepS == 0) w = "every tier needs a retention";
  }
  if (why) *why = w;
  return !w;
}

HistType tierType(const HistColumn& c, uint8_t tier) {
  if (tier == 0 || c.agg != HistAgg::Sum) return c.type;
  switch (c.type) {
    case HistType::I8:
    case HistType::I16: return HistType::I32;
    case HistType::U8:
    case HistType::U16: return HistType::U32;
    default: return c.type;
  }
}

bool layoutOf(const HistSeriesDef& def, uint8_t tier, Layout& out) {
  if (!validate(def, nullptr) || tier >= def.nTiers) return false;
  Layout l;
  uint32_t h = 2166136261u;
  const uint8_t version = kSegVersion;
  fnv(h, &version, 1);
  fnv(h, &def.tiers[tier].bucketS, 4);
  if (localTier(def, tier)) fnv(h, "local", 5);  // other bucket starts: other files
  size_t bytes = 0, values = 0;
  for (uint8_t i = 0; i < def.nCols; ++i) {
    const HistColumn& c = def.cols[i];
    l.types[i] = tierType(c, tier);
    bytes += histTypeSize(l.types[i]) * c.width;
    values += c.width;
    fnv(h, c.key, strlen(c.key));
    fnv(h, &c.width, 2);
    const uint8_t ty = uint8_t(l.types[i]), ag = uint8_t(c.agg);
    fnv(h, &ty, 1);
    fnv(h, &ag, 1);
    fnv(h, &c.scale, 4);
  }
  if (4 + bytes + 2 > 0xFFFF) return false;
  l.nValues = uint16_t(values);
  l.valueBytes = uint16_t(bytes);
  l.recSize = uint16_t(4 + bytes + 2);
  l.schemaHash = h;
  out = l;
  return true;
}

uint16_t crc16(const uint8_t* d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; ++i) {
    crc ^= uint16_t(d[i]) << 8;
    for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
  }
  return crc;
}

void encodeRecord(const HistSeriesDef& def, const Layout& lay, uint32_t t, const double* v, uint8_t* out) {
  const bool sentinel = !isPutSeries(def);
  putU32(out, t);
  uint8_t* p = out + 4;
  size_t k = 0;
  for (uint8_t i = 0; i < def.nCols; ++i) {
    const HistColumn& c = def.cols[i];
    const size_t sz = histTypeSize(lay.types[i]);
    for (uint16_t e = 0; e < c.width; ++e, ++k, p += sz) putValue(p, lay.types[i], c.scale, v[k], sentinel);
  }
  putU16(p, crc16(out, 4 + lay.valueBytes));
}

void encodeRaw(const Layout& lay, uint32_t t, const void* values, uint8_t* out) {
  putU32(out, t);
  memcpy(out + 4, values, lay.valueBytes);
  putU16(out + 4 + lay.valueBytes, crc16(out, 4 + lay.valueBytes));
}

bool recordTime(const uint8_t* rec, uint16_t recSize, uint32_t& t) {
  if (recSize < 6 || crc16(rec, recSize - 2) != getU16(rec + recSize - 2)) return false;
  t = getU32(rec);
  return true;
}

bool decodeRecord(const HistSeriesDef& def, const Layout& lay, const uint8_t* rec, uint32_t& t, double* v) {
  if (!recordTime(rec, lay.recSize, t)) return false;
  const bool sentinel = !isPutSeries(def);
  const uint8_t* p = rec + 4;
  size_t k = 0;
  for (uint8_t i = 0; i < def.nCols; ++i) {
    const HistColumn& c = def.cols[i];
    const size_t sz = histTypeSize(lay.types[i]);
    for (uint16_t e = 0; e < c.width; ++e, ++k, p += sz) v[k] = getValue(p, lay.types[i], c.scale, sentinel);
  }
  return true;
}

void encodeHeader(const SegHeader& h, uint8_t* out) {
  putU32(out, h.magic);
  out[4] = h.version;
  out[5] = h.tier;
  putU16(out + 6, h.recSize);
  putU32(out + 8, h.schemaHash);
  putU32(out + 12, h.bucketS);
  putU32(out + 16, h.startT);
}

bool decodeHeader(const uint8_t* in, size_t len, SegHeader& h) {
  if (len < kSegHeaderLen) return false;
  SegHeader r;
  r.magic = getU32(in);
  r.version = in[4];
  r.tier = in[5];
  r.recSize = getU16(in + 6);
  r.schemaHash = getU32(in + 8);
  r.bucketS = getU32(in + 12);
  r.startT = getU32(in + 16);
  if (r.magic != kSegMagic || r.version != kSegVersion || r.recSize < 6) return false;
  h = r;
  return true;
}

size_t validRecords(const uint8_t* data, size_t len, uint16_t recSize) {
  if (recSize < 6) return 0;
  size_t n = 0;
  uint32_t t;
  for (size_t at = 0; at + recSize <= len; at += recSize, ++n) {
    if (!recordTime(data + at, recSize, t)) break;
  }
  return n;
}

HistPut putCheck(uint32_t newest, uint32_t t) {
  if (t == newest && newest != 0) return HistPut::Duplicate;
  if (t < newest) return HistPut::Stale;
  return HistPut::Ok;
}

uint32_t bucketStart(uint32_t t, uint32_t bucketS) { return bucketS ? t - t % bucketS : t; }

bool localTier(const HistSeriesDef& def, uint8_t tier) {
  const uint32_t b = def.tiers[tier].bucketS;
  return def.localDays && b && b % 86400 == 0;
}

uint32_t tierStart(const HistSeriesDef& def, uint8_t tier, uint32_t t, UtcOffsetFn off) {
  const uint32_t b = def.tiers[tier].bucketS;
  if (!off || !localTier(def, tier)) return bucketStart(t, b);
  return localBucketStart(t, b, off);
}

uint32_t localBucketStart(uint32_t t, uint32_t b, UtcOffsetFn off) {
  if (!off || !b || b % 86400) return bucketStart(t, b);
  const int64_t local = int64_t(t) + off(t);
  const int64_t localStart = local - (local % b + b) % b;
  int64_t utc = localStart - off(t);        // with t's offset first,
  if (utc < 0) return 0;
  utc = localStart - off(uint32_t(utc));    // then the offset at that midnight (DST)
  if (utc < 0) return 0;
  return utc > t ? bucketStart(t, b) : uint32_t(utc);  // never after t
}

uint64_t tierNeed(const HistSeriesDef& def, uint8_t tier, uint16_t recSize) {
  const HistTier& z = def.tiers[tier];
  return z.bucketS ? uint64_t(z.keepS / z.bucketS + 1) * recSize : 0;
}

void splitByNeed(uint64_t cap, const uint64_t* need, size_t n, uint32_t* share) {
  double total = 0;
  for (size_t i = 0; i < n; ++i) total += need[i] ? double(need[i]) : 1.0;
  for (size_t i = 0; i < n; ++i) {
    const double s = total > 0 ? double(cap) * (need[i] ? double(need[i]) : 1.0) / total : 0;
    share[i] = s < 1 ? 1 : s >= 4294967295.0 ? 0xFFFFFFFFu : uint32_t(s);
  }
}

// ---- Bucket ----

void Bucket::attach(double* acc, uint32_t* cnt, uint16_t nValues) {
  acc_ = acc;
  cnt_ = cnt;
  n_ = nValues;
  reset(0);
}

void Bucket::reset(uint32_t start) {
  start_ = start;
  samples_ = 0;
  for (uint16_t i = 0; i < n_; ++i) {
    acc_[i] = 0;
    cnt_[i] = 0;
  }
}

void Bucket::add(const HistSeriesDef& def, const double* v) {
  size_t k = 0;
  for (uint8_t i = 0; i < def.nCols; ++i) {
    const HistColumn& c = def.cols[i];
    for (uint16_t e = 0; e < c.width; ++e, ++k) {
      const double x = v[k];
      if (isnan(x)) continue;  // an unknown value is not a number: it doesn't count
      double& a = acc_[k];
      uint32_t& n = cnt_[k];
      switch (c.agg) {
        case HistAgg::Sum:
        case HistAgg::Avg: a += x; break;
        case HistAgg::Min: a = n ? (x < a ? x : a) : x; break;
        case HistAgg::Max: a = n ? (x > a ? x : a) : x; break;
        case HistAgg::Last: a = x; break;
      }
      ++n;
    }
  }
  ++samples_;
}

void Bucket::finish(const HistSeriesDef& def, double* out) const {
  size_t k = 0;
  for (uint8_t i = 0; i < def.nCols; ++i) {
    const HistColumn& c = def.cols[i];
    for (uint16_t e = 0; e < c.width; ++e, ++k) {
      if (!cnt_[k]) {
        out[k] = NAN;  // no sample: unknown, also for a Sum
      } else {
        out[k] = c.agg == HistAgg::Avg ? acc_[k] / cnt_[k] : acc_[k];
      }
    }
  }
}

// ---- Downsampler ----

void Downsampler::attach(const HistSeriesDef& def, uint32_t res, double* acc, uint32_t* cnt, double* out,
                         uint16_t nValues, Emit emit, void* ctx, UtcOffsetFn off) {
  def_ = &def;
  res_ = res;
  off_ = off;
  b_.attach(acc, cnt, nValues);
  out_ = out;
  n_ = nValues;
  emit_ = emit;
  ctx_ = ctx;
  open_ = false;
}

void Downsampler::flush() {
  if (!open_) return;
  b_.finish(*def_, out_);
  emit_(ctx_, groupT_, out_, n_);
  open_ = false;
}

void Downsampler::feed(uint32_t t, const double* v) {
  const uint32_t g = res_ ? localBucketStart(t, res_, off_) : (open_ ? groupT_ : t);
  if (open_ && g != groupT_) flush();
  if (!open_) {
    b_.reset(g);
    groupT_ = g;
    open_ = true;
  }
  b_.add(*def_, v);
}

void Downsampler::finish() { flush(); }

// ---- retention ----

void retention(const SegInfo* segs, size_t n, uint32_t now, uint32_t keepS, uint32_t maxBytes, bool* drop) {
  uint64_t total = 0;
  for (size_t i = 0; i < n; ++i) {
    drop[i] = false;
    total += segs[i].bytes;
  }
  if (n == 0) return;
  const uint32_t horizon = now > keepS ? now - keepS : 0;
  for (size_t i = 0; i + 1 < n; ++i) {
    if (segs[i].newestT < horizon) {
      drop[i] = true;
      total -= segs[i].bytes;
    }
  }
  for (size_t i = 0; maxBytes && total > maxBytes && i + 1 < n; ++i) {
    if (drop[i]) continue;
    drop[i] = true;
    total -= segs[i].bytes;
  }
}

// ---- naming ----

uint32_t segmentKey(uint32_t t, uint32_t bucketS) {
  // days since the epoch -> civil date (Howard Hinnant's algorithm)
  const int64_t z = int64_t(t / 86400) + 719468;
  const int64_t era = z / 146097;
  const uint32_t doe = uint32_t(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  const uint32_t d = doy - (153 * mp + 2) / 5 + 1;
  const uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  const uint32_t y = uint32_t(int64_t(yoe) + era * 400 + (m <= 2 ? 1 : 0));
  if (bucketS == 0 || bucketS >= 86400) return y * 100 + m;
  return (y * 100 + m) * 100 + d;
}

double rowDerived(const double* v, uint16_t n, int code) {
  double acc = 0;
  uint16_t k = 0;
  for (uint16_t i = 0; i < n; ++i) {
    const double x = v[i];
    if (isnan(x)) continue;
    if (!k) acc = x;
    else if (code == kRowMin) acc = x < acc ? x : acc;
    else if (code == kRowMax) acc = x > acc ? x : acc;
    else acc += x;
    ++k;
  }
  if (!k) return NAN;
  return code == kRowAvg ? acc / k : acc;
}

bool findColumn(const HistSeriesDef& def, const char* spec, size_t specLen, uint8_t& col, int& element) {
  size_t keyLen = specLen;
  element = -1;
  for (size_t i = 0; i < specLen; ++i) {
    if (spec[i] != '[') continue;
    keyLen = i;
    if (specLen < i + 3 || spec[specLen - 1] != ']') return false;
    const char* in = spec + i + 1;
    const size_t inLen = specLen - i - 2;
    if (inLen == 3 && (!strncmp(in, "min", 3) || !strncmp(in, "avg", 3) || !strncmp(in, "max", 3))) {
      element = in[1] == 'i' ? kRowMin : in[1] == 'v' ? kRowAvg : kRowMax;
      break;
    }
    int e = 0;
    for (size_t j = i + 1; j + 1 < specLen; ++j) {
      if (spec[j] < '0' || spec[j] > '9') return false;
      e = e * 10 + (spec[j] - '0');
      if (e > 65535) return false;
    }
    element = e;
    break;
  }
  for (uint8_t i = 0; i < def.nCols; ++i) {
    if (strlen(def.cols[i].key) == keyLen && strncmp(def.cols[i].key, spec, keyLen) == 0) {
      if (element >= int(def.cols[i].width)) return false;
      col = i;
      return true;
    }
  }
  return false;
}

}  // namespace hist
}  // namespace ESPRack
