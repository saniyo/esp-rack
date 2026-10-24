#pragma once
#ifndef ESPRACK_HISTORY_HISTCORE_H
#define ESPRACK_HISTORY_HISTCORE_H

// The pure part of the history module (docs/plans/history.md): record layout,
// the segment format, aggregation, rollup, downsampling and retention.
// No file system, no Arduino, no allocation after setup: host-tested in
// test-host/test/test_history; modules/history drives it.
//
// Segment file: a 20-byte header, then fixed-size records.
//   header  'HSEG' u32 | version u8 | tier u8 | recSize u16 | schemaHash u32
//           | bucketS u32 | startT u32                       (little endian)
//   record  t u32 | values (each column's type, width elements, LE) | crc16
// crc16 is CRC-16/CCITT-FALSE over t and the values. A torn write leaves a
// bad tail: validRecords() stops at the first record whose crc fails, and the
// module truncates the file there at mount.

#include <stddef.h>
#include <stdint.h>

#include <history/IHistory.h>

namespace ESPRack {
namespace hist {

constexpr uint32_t kSegMagic = 0x47455348;  // "HSEG"
constexpr uint8_t kSegVersion = 1;
constexpr size_t kSegHeaderLen = 20;
constexpr size_t kMaxCols = 16;
constexpr size_t kMaxValues = 1024;  // the sum of the column widths
constexpr size_t kMaxTiers = 4;
constexpr size_t kMaxIdLen = 31;

struct SegHeader {
  uint32_t magic = kSegMagic;
  uint8_t version = kSegVersion;
  uint8_t tier = 0;
  uint16_t recSize = 0;
  uint32_t schemaHash = 0;
  uint32_t bucketS = 0;
  uint32_t startT = 0;
};

// One tier's record layout.
struct Layout {
  uint16_t nValues = 0;     // the sum of the column widths
  uint16_t valueBytes = 0;
  uint16_t recSize = 0;     // 4 + valueBytes + 2
  uint32_t schemaHash = 0;  // columns, widths, tier types, bucketS
  HistType types[kMaxCols];  // per column, on this tier
};

// A definition the module can keep; *why says what's wrong.
bool validate(const HistSeriesDef& def, const char** why);
bool isPutSeries(const HistSeriesDef& def);
// The column's type on `tier`: Sum columns widen to 32 bits on rollup tiers
// (I8/I16 -> I32, U8/U16 -> U32); the rest keep their type.
HistType tierType(const HistColumn& c, uint8_t tier);
bool layoutOf(const HistSeriesDef& def, uint8_t tier, Layout& out);

uint16_t crc16(const uint8_t* d, size_t n);

// Record <-> doubles (integer types: value * scale, rounded half away from
// zero and clamped to the type; decode divides by scale).
void encodeRecord(const HistSeriesDef& def, const Layout& lay, uint32_t t, const double* v, uint8_t* out);
// Raw values (put-series): the caller's bytes as they are, valueBytes long.
void encodeRaw(const Layout& lay, uint32_t t, const void* values, uint8_t* out);
bool decodeRecord(const HistSeriesDef& def, const Layout& lay, const uint8_t* rec, uint32_t& t, double* v);
bool recordTime(const uint8_t* rec, uint16_t recSize, uint32_t& t);  // false: bad crc

void encodeHeader(const SegHeader& h, uint8_t* out);
bool decodeHeader(const uint8_t* in, size_t len, SegHeader& h);
// Whole records from the start of `data` whose crc is good (stops at the first bad one).
size_t validRecords(const uint8_t* data, size_t len, uint16_t recSize);

// Put-series ordering: Ok when t is newer than `newest` (then newest = t).
HistPut putCheck(uint32_t newest, uint32_t t);
uint32_t bucketStart(uint32_t t, uint32_t bucketS);

// One open bucket: per value element, by its column's aggregate. The module
// gives it storage (PSRAM or internal) for `nValues` doubles and counters.
class Bucket {
 public:
  void attach(double* acc, uint32_t* cnt, uint16_t nValues);
  void reset(uint32_t start);
  void add(const HistSeriesDef& def, const double* v);
  // The aggregated values; elements that got no sample are NaN.
  void finish(const HistSeriesDef& def, double* out) const;
  bool empty() const { return samples_ == 0; }
  uint32_t start() const { return start_; }

 private:
  double* acc_ = nullptr;
  uint32_t* cnt_ = nullptr;
  uint16_t n_ = 0;
  uint32_t start_ = 0;
  uint32_t samples_ = 0;
};

// Groups ascending rows into `res`-second buckets (res 0: one group for
// everything) and emits one row per group, aggregated by the column aggs.
// The group's time is its start (res 0: the first row's time).
class Downsampler {
 public:
  using Emit = void (*)(void* ctx, uint32_t t, const double* v, uint16_t n);
  // acc/cnt: the group's accumulators, out: the emitted row; nValues each.
  void attach(const HistSeriesDef& def, uint32_t res, double* acc, uint32_t* cnt, double* out, uint16_t nValues,
              Emit emit, void* ctx);
  void feed(uint32_t t, const double* v);
  void finish();

 private:
  void flush();
  const HistSeriesDef* def_ = nullptr;
  uint32_t res_ = 0;
  Bucket b_;
  double* out_ = nullptr;
  uint16_t n_ = 0;
  Emit emit_ = nullptr;
  void* ctx_ = nullptr;
  bool open_ = false;
  uint32_t groupT_ = 0;
};

// Retention over one tier's segments, sorted oldest first. drop[i] = true for
// segments to delete: those whose newest record is older than now - keepS,
// then the oldest ones until the rest fits maxBytes (0 = no cap). The newest
// segment is never dropped.
struct SegInfo {
  uint32_t newestT;
  uint32_t bytes;
};
void retention(const SegInfo* segs, size_t n, uint32_t now, uint32_t keepS, uint32_t maxBytes, bool* drop);

// The segment a record goes to: YYYYMMDD (UTC) for buckets under a day,
// YYYYMM for daily or longer buckets and for put-series.
uint32_t segmentKey(uint32_t t, uint32_t bucketS);

// The schema-independent column lookup for queries: the column `key` and an
// optional element "key[5]". false when there is no such column / element.
bool findColumn(const HistSeriesDef& def, const char* spec, size_t specLen, uint8_t& col, int& element);

}  // namespace hist
}  // namespace ESPRack

#endif  // ESPRACK_HISTORY_HISTCORE_H
