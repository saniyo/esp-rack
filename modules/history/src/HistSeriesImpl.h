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

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <history/HistCore.h>
#include <history/IHistory.h>

namespace ESPRack {
namespace histmod {

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
  void mount(fs::FS& fs);
  size_t flush(fs::FS& fs, uint8_t* scratch, size_t scratchLen);  // bytes written
  void retention(fs::FS& fs, uint32_t now, uint32_t maxBytes);
  void erase(fs::FS& fs);

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
  void scanTier(fs::FS& fs, uint8_t tier);
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
  bool psram_ = false;
  uint32_t full_ = 0, stale_ = 0;
  uint32_t ramBytes_ = 0;
};

}  // namespace histmod
}  // namespace ESPRack

#endif  // ESPRACK_HIST_SERIES_IMPL_H
