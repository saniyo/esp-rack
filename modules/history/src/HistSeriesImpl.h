#pragma once
#ifndef ESPRACK_HIST_SERIES_IMPL_H
#define ESPRACK_HIST_SERIES_IMPL_H

// One registered series of modules/history (docs/plans/history.md): its RAM
// side (open buckets, rollups, rings of closed records waiting for flush)
// and its files (/hist/<id>/t<tier>/<key>-<hash8>.seg).
//
// Threads: add/put/putRaw/newestT/stats from any task under mx_ (RAM only).
// Until the "hist" task has mounted the series (about a second after it is
// registered) they return NotReady: the newest record must be known first.
// mount/flush/retention/erase run on the module's "hist" task; read() on the
// REST task. File work never holds mx_ (it copies records out first).
//
// Column keys, units and the series id passed in the definition must outlive
// the series (string literals); the column and tier arrays are copied.

#include <functional>
#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <history/HistCore.h>
#include <history/IHistory.h>

namespace ESPRack {
namespace histmod {

// Seconds east of UTC at `utc` in the device's time zone (TimeSync owns TZ).
int32_t utcOffsetAt(uint32_t utc);

// A row read back: t and the decoded values (nValues of the tier's layout).
using RowFn = bool (*)(void* ctx, uint32_t t, const double* v);

class Series : public IHistorySeries {
 public:
  ~Series();
  // Copies the definition and allocates the buffers (PSRAM when there is
  // some). false + *why when it can't.
  bool init(const HistSeriesDef& def, bool psram, uint32_t flushS, const char** why);

  // ---- IHistorySeries ----
  HistPut add(uint32_t t, const double* v, size_t n) override;
  HistPut put(uint32_t t, const double* v, size_t n) override;
  HistPut putRaw(uint32_t t, const void* rec, size_t len) override;
  uint32_t newestT() const override;
  HistSeriesStats stats() const override;

  const HistSeriesDef& def() const { return def_; }
  const char* id() const { return id_; }
  uint32_t schemaHash(uint8_t tier) const { return tier_[tier].lay.schemaHash; }
  uint16_t nValues(uint8_t tier) const { return tier_[tier].lay.nValues; }
  bool mounted() const { return mounted_; }
  uint32_t queuedBytes() const;
  uint32_t ramBytes() const { return ramBytes_; }
  // Most of a tier's ring is used: flush soon.
  bool pressing() const;

  // ---- the "hist" task ----
  // Around each flash write of mount / retention / erase (a file removed, a
  // torn tail cut): take() waits for the caller's write gate slot for that
  // step (power/WriteGate.h), done() gives it back once the write is over
  // (the reads that follow hold no slot). take() false: not now
  // (PowerGuard): the step is skipped.
  struct Step {
    std::function<bool()> take;
    std::function<void()> done;
  };
  // Reads without a step; only cutting a torn tail (after a power cut)
  // takes one. false: that step was refused, the series stays unmounted.
  bool mount(fs::FS& fs, const Step& step);
  // Bytes written. budget: stop after this many (a chunk of up to
  // scratchLen is never split): 1 = one chunk, a write gate slot each.
  size_t flush(fs::FS& fs, uint8_t* scratch, size_t scratchLen, size_t budget = SIZE_MAX);
  // A step per file removed; a refused step ends the pass (the next one goes on).
  void retention(fs::FS& fs, uint32_t now, uint32_t maxBytes, const Step& step);
  // Bytes for the whole retention of all tiers (a put-series: what it has
  // on file plus its buffer), the weight of its share of the global cap.
  uint64_t needBytes() const;
  // A step per file / directory removed. false: a step was refused, files
  // are left: erase again later. The RAM state goes at the first call; until
  // the files are gone too, flush() writes nothing (erasing()).
  bool erase(fs::FS& fs, const Step& step);
  bool erasing() const { return erasing_; }

  // ---- queries (any task) ----
  // The finest tier whose retention still covers `from` (else the coarsest).
  uint8_t pickTier(uint32_t from, uint32_t now) const;
  // Rows of `tier` with from <= t <= to, ascending: the files, then the
  // records still queued in RAM. fn returns false to stop.
  void read(fs::FS& fs, uint8_t tier, uint32_t from, uint32_t to, double* v, uint8_t* rec, RowFn fn,
            void* ctx) const;

 private:
  struct Tier {
    hist::Layout lay;
    hist::Bucket bucket;           // open bucket (bucketed tiers)
    double* acc = nullptr;
    uint32_t* cnt = nullptr;
    double* out = nullptr;         // a closing bucket's values (rollup tiers)
    uint8_t* q = nullptr;          // ring of whole records
    uint32_t qCap = 0, qHead = 0, qLen = 0;  // bytes
    uint32_t fileNewest = 0;       // newest record on file
    uint32_t queuedNewest = 0;     // newest record queued (or on file)
    uint32_t oldestT = 0, records = 0, bytes = 0, oldBytes = 0;
  };

  void dirOf(uint8_t tier, char* out, size_t len) const;
  bool enqueueLocked(uint8_t tier, const uint8_t* rec);
  // A closed bucket / record of `tier`: queue it and roll it up.
  void closedLocked(uint8_t tier, uint32_t t, const double* v);
  void rollupLocked(uint8_t tier, uint32_t t, const double* v);
  // false: a torn tail that needs cutting, and the step was refused.
  bool scanTier(fs::FS& fs, uint8_t tier, const Step& step);
  void rebuildRollup(fs::FS& fs, uint8_t tier);

  char id_[hist::kMaxIdLen + 1] = "";
  HistColumn cols_[hist::kMaxCols];
  HistTier tiers_[hist::kMaxTiers];
  HistSeriesDef def_{};
  Tier tier_[hist::kMaxTiers];
  double* tmp_ = nullptr;     // nValues of tier 0
  uint8_t* rec_ = nullptr;    // one record of the widest tier
  mutable SemaphoreHandle_t mx_ = nullptr;
  bool mounted_ = false;
  bool erasing_ = false;     // an erase is under way (hist task only)
  uint8_t flushFrom_ = 0;    // the tier a budgeted flush() serves first (they take turns)
  bool dirMade_[hist::kMaxTiers] = {};  // a tier's directory exists (mkdirs once, not per flush)
  bool psram_ = false;
  uint32_t full_ = 0, stale_ = 0;
  uint32_t ramBytes_ = 0;
};

}  // namespace histmod
}  // namespace ESPRack

#endif  // ESPRACK_HIST_SERIES_IMPL_H
