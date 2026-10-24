#pragma once
#ifndef ESPRACK_HISTORY_IHISTORY_H
#define ESPRACK_HISTORY_IHISTORY_H

// History: time series kept on flash or an SD card without wearing the flash
// out, served for charts, bars, pies and tables over a period the page picks
// (docs/plans/history.md). Late-bound like the other providers
// (App::history()); the module that keeps the files is modules/history.
//
//   static const ESPRack::HistColumn kCols[] = {
//     {"energy", "Wh", ESPRack::HistAgg::Sum, 8, ESPRack::HistType::U32},  // 8 channels
//     {"temp", "C", ESPRack::HistAgg::Avg}};
//   static const ESPRack::HistTier kTiers[] = {{3600, 7 * 86400}, {86400, 180 * 86400}};
//   ESPRack::HistSeriesDef def{"meter.main", "Main meter", kCols, 2, kTiers, 2};
//   series_ = app->history()->series(def, &why);        // any time, also from begin()
//   series_->add(now, values, 9);                        // every reading: summed/averaged into the hour
//
// Two kinds of series:
//   * bucketed (tier 0 bucketS > 0): add() samples into the open bucket, or
//     put() a finished one; closed buckets roll up into the next tiers;
//   * put-series (tier 0 bucketS == 0, one tier): every put()/putRaw() is a
//     record with the caller's time; t <= newestT() is Duplicate/Stale and
//     writes nothing (re-appending after a reboot is safe).
//
// Threads: series(), add(), put(), putRaw() from any task; they only touch
// RAM under a short lock. Files are written by the module's own task, in
// batches, while PowerGuard allows writes. An unexpected reset loses at most
// what was not flushed yet (flush interval) and the open buckets.
//
// No file system or Arduino types here: the core stays host-testable.

#include <stddef.h>
#include <stdint.h>

namespace ESPRack {

enum class HistAgg : uint8_t { Sum, Avg, Min, Max, Last };
enum class HistType : uint8_t { I8, U8, I16, U16, I32, U32, I64, U64, F32 };
enum class HistPut : uint8_t {
  Ok,
  Duplicate,  // put-series: t == newestT(): already there, nothing written
  Stale,      // t older than the newest record (clock stepped back), nothing written
  Full,       // the series' buffer is full: try again later (nothing dropped)
  NotReady,   // no valid time, or the module isn't running
  BadArgs     // wrong value count / record length
};

const char* histAggName(HistAgg a);    // "sum", "avg", "min", "max", "last"
const char* histTypeName(HistType t);  // "i8" ... "f32"
const char* histPutName(HistPut p);    // "ok", "duplicate", ...
size_t histTypeSize(HistType t);       // 1, 2, 4 or 8

struct HistColumn {
  const char* key;   // stable, lowercase LDH (a query names it: cols=in_mah)
  const char* unit;  // "" when none
  HistAgg agg;
  uint16_t width = 1;            // > 1: an array, e.g. one value per cell
  HistType type = HistType::F32;
  float scale = 1;               // integer types store value * scale (rounded)
};

// bucketS 0: one record per put (a put-series; only one tier).
struct HistTier {
  uint32_t bucketS;
  uint32_t keepS;    // retention of this tier, seconds
};

struct HistSeriesDef {
  const char* id;     // lowercase LDH + dots, stable: the file names and the REST path
  const char* title;
  const HistColumn* cols;
  uint8_t nCols;
  const HistTier* tiers;
  uint8_t nTiers;
  uint32_t maxBytes = 0;  // 0: a share of the module's global cap
};

struct HistSeriesStats {
  uint32_t newestT = 0;
  uint32_t oldestT = 0;
  uint32_t records = 0;     // on file, all tiers
  uint32_t bytes = 0;       // on file, all tiers, current schema
  uint32_t oldBytes = 0;    // on file in an older schema (readable, not mixed in)
  uint32_t queued = 0;      // closed records waiting for the next flush
  uint32_t full = 0;        // put() refused with Full / add() overflows
  uint32_t stale = 0;       // refused for an old time
};

class IHistorySeries {
 public:
  // A sample into the open bucket of tier 0: each column element is
  // aggregated by its column's HistAgg. n = the sum of the column widths.
  virtual HistPut add(uint32_t t, const double* v, size_t n) = 0;
  // A finished bucket (bucketed series: t = any time inside it) or a record
  // (put-series).
  virtual HistPut put(uint32_t t, const double* v, size_t n) = 0;
  // Put-series only: the record exactly as the columns lay it out (little
  // endian, no scale): exact integers of any width.
  virtual HistPut putRaw(uint32_t t, const void* rec, size_t len) = 0;
  virtual uint32_t newestT() const = 0;  // 0: empty
  virtual HistSeriesStats stats() const = 0;

 protected:
  ~IHistorySeries() = default;
};

class IHistoryProvider {
 public:
  virtual ~IHistoryProvider() = default;
  // Registers (or re-registers with a changed schema) a series. nullptr and
  // *why when refused (bad definition, no room). Any time.
  virtual IHistorySeries* series(const HistSeriesDef& def, const char** why = nullptr) = 0;
  virtual IHistorySeries* find(const char* id) = 0;
  // Files are mounted and the writer runs.
  virtual bool ready() const = 0;
};

}  // namespace ESPRack

#endif  // ESPRACK_HISTORY_IHISTORY_H
