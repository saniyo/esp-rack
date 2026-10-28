#include "HistSeriesImpl.h"

#include <esp_heap_caps.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace ESPRack {
namespace histmod {

int32_t utcOffsetAt(uint32_t utc) {
  const time_t t = time_t(utc);
  struct tm g, l;
  gmtime_r(&t, &g);
  localtime_r(&t, &l);
  int32_t days = l.tm_year != g.tm_year ? (l.tm_year > g.tm_year ? 1 : -1) : l.tm_yday - g.tm_yday;
  return days * 86400 + (l.tm_hour - g.tm_hour) * 3600 + (l.tm_min - g.tm_min) * 60 + (l.tm_sec - g.tm_sec);
}

namespace {

constexpr uint32_t kMinValidT = 1577836800;  // 2020-01-01: before it the clock isn't set
constexpr uint32_t kPutRecords = 16;         // a put-series' ring: its reserve
constexpr uint32_t kRollupRecords = 4;

struct Lock {
  explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(m_); }
  SemaphoreHandle_t m_;
};

void* alloc(size_t n, bool psram, uint32_t& total) {
  void* p = psram ? heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
  if (!p) p = calloc(1, n);
  if (p) total += n;
  return p;
}

// "20251024-a1b2c3d4.seg" -> key 20251024, hash 0xa1b2c3d4.
bool parseName(const char* name, uint32_t& key, uint32_t& hash) {
  const char* dash = strchr(name, '-');
  const char* dot = strrchr(name, '.');
  if (!dash || !dot || dot < dash || strcmp(dot, ".seg") != 0) return false;
  key = strtoul(name, nullptr, 10);
  hash = strtoul(dash + 1, nullptr, 16);
  return key != 0;
}

// The .seg files of a directory, sorted by name (the key comes first and has
// a fixed length per tier, so name order is time order).
std::vector<String> segments(fs::FS& fs, const char* dir) {
  std::vector<String> out;
  File d = fs.open(dir);
  if (!d || !d.isDirectory()) return out;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    const char* n = f.name();
    const char* base = strrchr(n, '/');
    base = base ? base + 1 : n;
    uint32_t k, h;
    if (!f.isDirectory() && parseName(base, k, h)) out.push_back(String(base));
  }
  std::sort(out.begin(), out.end(), [](const String& a, const String& b) { return a < b; });
  return out;
}

bool readHeader(File& f, hist::SegHeader& h) {
  uint8_t b[hist::kSegHeaderLen];
  f.seek(0);
  return f.read(b, sizeof b) == sizeof b && hist::decodeHeader(b, sizeof b, h);
}

// The time of record `i` (0 = first); false when it can't be read or its crc fails.
bool recordAt(File& f, uint16_t recSize, size_t i, uint8_t* rec, uint32_t& t) {
  if (!f.seek(hist::kSegHeaderLen + i * recSize)) return false;
  if (f.read(rec, recSize) != recSize) return false;
  return hist::recordTime(rec, recSize, t);
}

void mkdirs(fs::FS& fs, const char* id, const char* dir) {
  char p[48];
  fs.mkdir("/hist");
  snprintf(p, sizeof p, "/hist/%s", id);
  fs.mkdir(p);
  fs.mkdir(dir);
}

}  // namespace

Series::~Series() {
  for (Tier& z : tier_) {
    free(z.acc);
    free(z.cnt);
    free(z.out);
    free(z.q);
  }
  free(tmp_);
  free(rec_);
  if (mx_) vSemaphoreDelete(mx_);
}

bool Series::init(const HistSeriesDef& def, bool psram, uint32_t flushS, const char** why) {
  if (!hist::validate(def, why)) return false;
  strlcpy(id_, def.id, sizeof id_);
  memcpy(cols_, def.cols, sizeof(HistColumn) * def.nCols);
  memcpy(tiers_, def.tiers, sizeof(HistTier) * def.nTiers);
  def_ = def;
  def_.id = id_;
  def_.cols = cols_;
  def_.tiers = tiers_;
  psram_ = psram;
  const bool putSeries = hist::isPutSeries(def_);
  uint16_t widest = 0;
  for (uint8_t k = 0; k < def_.nTiers; ++k) {
    Tier& z = tier_[k];
    if (!hist::layoutOf(def_, k, z.lay)) {
      if (why) *why = "bad layout";
      return false;
    }
    widest = std::max(widest, z.lay.recSize);
    uint32_t recs = kRollupRecords;
    if (putSeries) {
      recs = kPutRecords;
    } else if (k == 0) {
      recs = 2 * flushS / tiers_[0].bucketS + 8;
    }
    z.qCap = recs * z.lay.recSize;
    z.q = static_cast<uint8_t*>(alloc(z.qCap, psram, ramBytes_));
    if (!putSeries) {
      z.acc = static_cast<double*>(alloc(sizeof(double) * z.lay.nValues, psram, ramBytes_));
      z.cnt = static_cast<uint32_t*>(alloc(sizeof(uint32_t) * z.lay.nValues, psram, ramBytes_));
      z.out = static_cast<double*>(alloc(sizeof(double) * z.lay.nValues, psram, ramBytes_));
      if (z.acc && z.cnt) z.bucket.attach(z.acc, z.cnt, z.lay.nValues);
    }
    if (!z.q || (!putSeries && (!z.acc || !z.cnt || !z.out))) {
      if (why) *why = "out of memory";
      return false;
    }
  }
  tmp_ = static_cast<double*>(alloc(sizeof(double) * tier_[0].lay.nValues, psram, ramBytes_));
  rec_ = static_cast<uint8_t*>(alloc(widest, psram, ramBytes_));
  mx_ = xSemaphoreCreateMutex();
  if (!tmp_ || !rec_ || !mx_) {
    if (why) *why = "out of memory";
    return false;
  }
  return true;
}

void Series::dirOf(uint8_t tier, char* out, size_t len) const { snprintf(out, len, "/hist/%s/t%u", id_, tier); }

bool Series::enqueueLocked(uint8_t tier, const uint8_t* rec) {
  Tier& z = tier_[tier];
  const uint16_t rs = z.lay.recSize;
  if (z.qLen + rs > z.qCap) return false;
  memcpy(z.q + (z.qHead + z.qLen) % z.qCap, rec, rs);  // qCap is a whole number of records
  z.qLen += rs;
  return true;
}

void Series::closedLocked(uint8_t tier, uint32_t t, const double* v) {
  Tier& z = tier_[tier];
  hist::encodeRecord(def_, z.lay, t, v, rec_);
  if (enqueueLocked(tier, rec_)) {
    z.queuedNewest = t;
  } else {
    ++full_;
  }
  if (tier + 1 < def_.nTiers) rollupLocked(tier + 1, t, v);
}

void Series::rollupLocked(uint8_t tier, uint32_t t, const double* v) {
  Tier& z = tier_[tier];
  const uint32_t bs = hist::tierStart(def_, tier, t, &utcOffsetAt);
  if (z.queuedNewest && bs <= z.queuedNewest) return;  // already rolled up (on file)
  if (!z.bucket.empty() && z.bucket.start() != bs) {
    if (bs < z.bucket.start()) return;
    z.bucket.finish(def_, z.out);
    closedLocked(tier, z.bucket.start(), z.out);
    z.bucket.reset(bs);
  } else if (z.bucket.empty() && z.bucket.start() != bs) {
    z.bucket.reset(bs);
  }
  z.bucket.add(def_, v);
}

HistPut Series::add(uint32_t t, const double* v, size_t n) {
  if (hist::isPutSeries(def_) || !v || n != tier_[0].lay.nValues) return HistPut::BadArgs;
  if (t < kMinValidT) return HistPut::NotReady;
  Lock l(mx_);
  if (!mounted_) return HistPut::NotReady;
  Tier& z = tier_[0];
  const uint32_t bs = hist::tierStart(def_, 0, t, &utcOffsetAt);
  if (z.queuedNewest && bs <= z.queuedNewest) {
    ++stale_;
    return HistPut::Stale;
  }
  if (!z.bucket.empty() && z.bucket.start() != bs) {
    if (bs < z.bucket.start()) {
      ++stale_;
      return HistPut::Stale;
    }
    z.bucket.finish(def_, tmp_);
    closedLocked(0, z.bucket.start(), tmp_);
    z.bucket.reset(bs);
  } else if (z.bucket.empty() && z.bucket.start() != bs) {
    z.bucket.reset(bs);
  }
  z.bucket.add(def_, v);
  return HistPut::Ok;
}

HistPut Series::put(uint32_t t, const double* v, size_t n) {
  if (!v || n != tier_[0].lay.nValues) return HistPut::BadArgs;
  Lock l(mx_);
  if (!mounted_) return HistPut::NotReady;
  Tier& z = tier_[0];
  const uint32_t at = hist::isPutSeries(def_) ? t : hist::tierStart(def_, 0, t, &utcOffsetAt);
  const HistPut c = hist::putCheck(z.queuedNewest, at);
  if (c != HistPut::Ok) {
    if (c == HistPut::Stale) ++stale_;
    return c;
  }
  if (z.qLen + z.lay.recSize > z.qCap) {
    ++full_;
    return HistPut::Full;
  }
  closedLocked(0, at, v);
  return HistPut::Ok;
}

HistPut Series::putRaw(uint32_t t, const void* rec, size_t len) {
  Tier& z = tier_[0];
  if (!hist::isPutSeries(def_) || !rec || len != z.lay.valueBytes) return HistPut::BadArgs;
  Lock l(mx_);
  if (!mounted_) return HistPut::NotReady;
  const HistPut c = hist::putCheck(z.queuedNewest, t);
  if (c != HistPut::Ok) {
    if (c == HistPut::Stale) ++stale_;
    return c;
  }
  hist::encodeRaw(z.lay, t, rec, rec_);
  if (!enqueueLocked(0, rec_)) {
    ++full_;
    return HistPut::Full;
  }
  z.queuedNewest = t;
  return HistPut::Ok;
}

uint32_t Series::newestT() const {
  Lock l(mx_);
  return tier_[0].queuedNewest;
}

HistSeriesStats Series::stats() const {
  HistSeriesStats s;
  Lock l(mx_);
  s.newestT = tier_[0].queuedNewest;
  for (uint8_t k = 0; k < def_.nTiers; ++k) {
    const Tier& z = tier_[k];
    uint32_t oldest = z.oldestT;
    // Nothing on file yet (a fresh device, before the first flush): the
    // oldest record still queued in RAM.
    if (!oldest && z.qLen >= z.lay.recSize) (void)hist::recordTime(z.q + z.qHead, z.lay.recSize, oldest);
    if (oldest && (!s.oldestT || oldest < s.oldestT)) s.oldestT = oldest;
    s.records += z.records;
    s.bytes += z.bytes;
    s.oldBytes += z.oldBytes;
    s.queued += z.qLen / z.lay.recSize;
  }
  s.full = full_;
  s.stale = stale_;
  return s;
}

uint32_t Series::queuedBytes() const {
  Lock l(mx_);
  uint32_t n = 0;
  for (uint8_t k = 0; k < def_.nTiers; ++k) n += tier_[k].qLen;
  return n;
}

bool Series::pressing() const {
  Lock l(mx_);
  for (uint8_t k = 0; k < def_.nTiers; ++k) {
    if (tier_[k].qLen * 4 > tier_[k].qCap * 3) return true;
  }
  return false;
}

// ---- files ----

bool Series::scanTier(fs::FS& fs, uint8_t tier, const Step& step) {
  Tier& z = tier_[tier];
  char dir[48];
  dirOf(tier, dir, sizeof dir);
  uint32_t records = 0, bytes = 0, oldBytes = 0, oldest = 0, newest = 0;
  const std::vector<String> names = segments(fs, dir);
  int lastCurrent = -1;
  for (size_t i = 0; i < names.size(); ++i) {
    uint32_t key, hash;
    if (parseName(names[i].c_str(), key, hash) && hash == z.lay.schemaHash) lastCurrent = int(i);
  }
  for (size_t i = 0; i < names.size(); ++i) {
    char path[80];
    snprintf(path, sizeof path, "%s/%s", dir, names[i].c_str());
    File f = fs.open(path, "r");
    if (!f) continue;
    hist::SegHeader h;
    const size_t size = f.size();
    if (!readHeader(f, h) || h.schemaHash != z.lay.schemaHash || h.recSize != z.lay.recSize) {
      oldBytes += size;
      continue;
    }
    size_t n = size > hist::kSegHeaderLen ? (size - hist::kSegHeaderLen) / h.recSize : 0;
    if (int(i) == lastCurrent) {
      // The newest file is the only one a power cut can tear: keep its whole records.
      size_t good = 0;
      uint32_t t;
      while (good < n && recordAt(f, h.recSize, good, rec_, t)) ++good;
      const bool torn = good < n || hist::kSegHeaderLen + n * h.recSize != size;
      // Cutting it copies the whole file: one step (it only follows a power
      // cut). Refused: appending after a torn record would misalign it.
      if (torn && !step.take()) return false;
      if (torn) {
        char tmp[84];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        File o = fs.open(tmp, "w");
        bool ok = !!o;
        uint8_t hb[hist::kSegHeaderLen];
        hist::encodeHeader(h, hb);
        if (ok) ok = o.write(hb, sizeof hb) == sizeof hb;
        for (size_t r = 0; ok && r < good; ++r) {
          ok = f.seek(hist::kSegHeaderLen + r * h.recSize) && f.read(rec_, h.recSize) == h.recSize &&
               o.write(rec_, h.recSize) == h.recSize;
        }
        if (o) o.close();
        f.close();
        if (ok && fs.remove(path) && fs.rename(tmp, path)) {
          log_w("[hist] %s: cut a torn tail (%u of %u records kept)", path, unsigned(good), unsigned(n));
        } else {
          fs.remove(tmp);
          log_e("[hist] %s: could not cut its torn tail", path);
        }
        step.done();
        n = good;
        f = fs.open(path, "r");
        if (!f) continue;
      }
      uint32_t t2;
      if (n && recordAt(f, h.recSize, n - 1, rec_, t2)) newest = t2;
    }
    if (!oldest && n) {
      uint32_t t0;
      if (recordAt(f, h.recSize, 0, rec_, t0)) oldest = t0;
    }
    records += n;
    bytes += hist::kSegHeaderLen + n * h.recSize;
  }
  Lock l(mx_);
  z.records = records;
  z.bytes = bytes;
  z.oldBytes = oldBytes;
  z.oldestT = oldest;
  z.fileNewest = newest;
  if (newest > z.queuedNewest) z.queuedNewest = newest;
  return true;
}

void Series::rebuildRollup(fs::FS& fs, uint8_t tier) {
  // Lower-tier records newer than this tier's newest bucket go into its open
  // bucket again (and close any older buckets on the way).
  // (+1, not + bucketS: a local day can be 23 or 25 h; rollupLocked skips
  // what the newest bucket already holds.)
  const uint32_t from = tier_[tier].fileNewest ? tier_[tier].fileNewest + 1 : 0;
  std::vector<double> v(tier_[tier - 1].lay.nValues);
  std::vector<uint8_t> rec(tier_[tier - 1].lay.recSize);
  struct Ctx {
    Series* self;
    uint8_t tier;
  } ctx{this, tier};
  read(fs, tier - 1, from, 0xFFFFFFFF, v.data(), rec.data(),
       [](void* c, uint32_t t, const double* val) {
         Ctx* x = static_cast<Ctx*>(c);
         Lock l(x->self->mx_);
         x->self->rollupLocked(x->tier, t, val);
         return true;
       },
       &ctx);
}

bool Series::mount(fs::FS& fs, const Step& step) {
  for (uint8_t k = 0; k < def_.nTiers; ++k) {
    if (!scanTier(fs, k, step)) return false;
  }
  for (uint8_t k = 1; k < def_.nTiers; ++k) rebuildRollup(fs, k);
  Lock l(mx_);
  mounted_ = true;
  return true;
}

size_t Series::flush(fs::FS& fs, uint8_t* scratch, size_t scratchLen, size_t budget) {
  size_t written = 0;
  if (erasing_) return 0;  // its files go: nothing appends to them meanwhile
  for (uint8_t i = 0; i < def_.nTiers; ++i) {
    const uint8_t k = uint8_t((flushFrom_ + i) % def_.nTiers);
    Tier& z = tier_[k];
    const uint16_t rs = z.lay.recSize;
    char dir[48];
    dirOf(k, dir, sizeof dir);
    for (;;) {
      size_t n = 0;
      uint32_t fileNewest;
      {
        Lock l(mx_);
        fileNewest = z.fileNewest;
        const size_t want = std::min<size_t>(z.qLen, (scratchLen / rs) * rs);
        for (size_t at = 0; at < want; at += rs) memcpy(scratch + at, z.q + (z.qHead + at) % z.qCap, rs);
        n = want;
      }
      if (!n) break;
      // Runs of records with the same segment key go to one file.
      size_t done = 0;
      bool ok = true;
      while (done < n && ok) {
        uint32_t t0 = 0;
        hist::recordTime(scratch + done, rs, t0);
        const uint32_t key = hist::segmentKey(t0, tiers_[k].bucketS);
        size_t end = done;
        uint32_t last = 0;
        while (end < n) {
          uint32_t t = 0;
          hist::recordTime(scratch + end, rs, t);
          if (hist::segmentKey(t, tiers_[k].bucketS) != key) break;
          last = t;
          end += rs;
        }
        // Skip records already on file (queued again before a remount).
        size_t from = done;
        while (from < end) {
          uint32_t t = 0;
          hist::recordTime(scratch + from, rs, t);
          if (t > fileNewest) break;
          from += rs;
        }
        if (from < end) {
          mkdirs(fs, id_, dir);
          char path[80];
          snprintf(path, sizeof path, "%s/%lu-%08lx.seg", dir, (unsigned long)key, (unsigned long)z.lay.schemaHash);
          File f = fs.open(path, "a");
          ok = !!f;
          if (ok && f.size() == 0) {
            hist::SegHeader h;
            h.tier = k;
            h.recSize = rs;
            h.schemaHash = z.lay.schemaHash;
            h.bucketS = tiers_[k].bucketS;
            hist::recordTime(scratch + from, rs, h.startT);
            uint8_t hb[hist::kSegHeaderLen];
            hist::encodeHeader(h, hb);
            ok = f.write(hb, sizeof hb) == sizeof hb;
            if (ok) {
              Lock l(mx_);
              z.bytes += sizeof hb;
            }
          }
          if (ok) ok = f.write(scratch + from, end - from) == end - from;
          if (f) f.close();
          if (ok) {
            Lock l(mx_);
            const uint32_t recs = uint32_t((end - from) / rs);
            z.records += recs;
            z.bytes += recs * rs;
            if (!z.oldestT) hist::recordTime(scratch + from, rs, z.oldestT);
            z.fileNewest = last;
            fileNewest = last;
            written += end - from;
          }
        }
        if (ok) done = end;
      }
      Lock l(mx_);
      z.qHead = uint32_t((z.qHead + done) % z.qCap);
      z.qLen -= uint32_t(done);
      if (!ok) {
        log_e("[hist] %s: write failed, kept in RAM", id_);
        return written;
      }
      if (written >= budget) {
        flushFrom_ = uint8_t((k + 1) % def_.nTiers);  // the next call starts with the next tier
        return written;
      }
    }
  }
  return written;
}

uint64_t Series::needBytes() const {
  if (hist::isPutSeries(def_)) {
    Lock l(mx_);
    return uint64_t(tier_[0].bytes) + tier_[0].qCap;
  }
  uint64_t need = 0;
  for (uint8_t k = 0; k < def_.nTiers; ++k) need += hist::tierNeed(def_, k, tier_[k].lay.recSize);
  return need;
}

void Series::retention(fs::FS& fs, uint32_t now, uint32_t maxBytes, const Step& step) {
  // The series' bytes go to its tiers by what each needs for its retention.
  uint32_t tierCap[hist::kMaxTiers] = {};
  if (maxBytes) {
    uint64_t need[hist::kMaxTiers];
    for (uint8_t k = 0; k < def_.nTiers; ++k) need[k] = hist::tierNeed(def_, k, tier_[k].lay.recSize);
    if (hist::isPutSeries(def_)) need[0] = 1;
    hist::splitByNeed(maxBytes, need, def_.nTiers, tierCap);
  }
  for (uint8_t k = 0; k < def_.nTiers; ++k) {
    char dir[48];
    dirOf(k, dir, sizeof dir);
    const std::vector<String> names = segments(fs, dir);
    if (names.empty()) continue;
    std::vector<hist::SegInfo> info(names.size());
    for (size_t i = 0; i < names.size(); ++i) {
      char path[80];
      snprintf(path, sizeof path, "%s/%s", dir, names[i].c_str());
      File f = fs.open(path, "r");
      hist::SegHeader h;
      info[i] = {0, 0};
      if (!f) continue;
      info[i].bytes = f.size();
      if (readHeader(f, h) && f.size() >= hist::kSegHeaderLen + h.recSize) {
        uint32_t t;
        std::vector<uint8_t> rec(h.recSize);
        if (recordAt(f, h.recSize, (f.size() - hist::kSegHeaderLen) / h.recSize - 1, rec.data(), t)) info[i].newestT = t;
      }
    }
    std::unique_ptr<bool[]> d(new bool[names.size()]);
    hist::retention(info.data(), info.size(), now, tiers_[k].keepS, tierCap[k], d.get());
    bool removed = false;
    bool refused = false;
    for (size_t i = 0; i < names.size(); ++i) {
      if (!d[i]) continue;
      if (!step.take()) {
        refused = true;
        break;
      }
      char path[80];
      snprintf(path, sizeof path, "%s/%s", dir, names[i].c_str());
      removed |= fs.remove(path);
      step.done();
    }
    if (removed) (void)scanTier(fs, k, step);
    if (refused) return;
  }
}

bool Series::erase(fs::FS& fs, const Step& step) {
  // The RAM state goes at the first call (not again on a retry: records
  // taken since stay); the files a step at a time.
  for (uint8_t k = 0; !erasing_ && k < def_.nTiers; ++k) {
    Lock l(mx_);
    Tier& z = tier_[k];
    z.records = z.bytes = z.oldBytes = z.oldestT = 0;
    z.fileNewest = 0;
    z.queuedNewest = 0;
    z.qHead = z.qLen = 0;
    if (z.acc) z.bucket.reset(0);
  }
  erasing_ = true;
  for (uint8_t k = 0; k < def_.nTiers; ++k) {
    char dir[48];
    dirOf(k, dir, sizeof dir);
    for (const String& n : segments(fs, dir)) {
      if (!step.take()) return false;
      char path[80];
      snprintf(path, sizeof path, "%s/%s", dir, n.c_str());
      fs.remove(path);
      step.done();
    }
    if (fs.exists(dir)) {
      if (!step.take()) return false;
      fs.rmdir(dir);
      step.done();
    }
  }
  char p[48];
  snprintf(p, sizeof p, "/hist/%s", id_);
  if (fs.exists(p)) {
    if (!step.take()) return false;
    fs.rmdir(p);
    step.done();
  }
  erasing_ = false;
  return true;
}

uint8_t Series::pickTier(uint32_t from, uint32_t now) const {
  for (uint8_t k = 0; k < def_.nTiers; ++k) {
    const uint32_t horizon = now > tiers_[k].keepS ? now - tiers_[k].keepS : 0;
    if (from >= horizon) return k;
  }
  return uint8_t(def_.nTiers - 1);
}

void Series::read(fs::FS& fs, uint8_t tier, uint32_t from, uint32_t to, double* v, uint8_t* rec, RowFn fn,
                  void* ctx) const {
  const Tier& z = tier_[tier];
  const uint16_t rs = z.lay.recSize;
  char dir[48];
  dirOf(tier, dir, sizeof dir);
  const uint32_t kFrom = hist::segmentKey(from, tiers_[tier].bucketS);
  const uint32_t kTo = hist::segmentKey(to, tiers_[tier].bucketS);
  uint32_t lastOnFile = 0;
  for (const String& n : segments(fs, dir)) {
    uint32_t key, hash;
    if (!parseName(n.c_str(), key, hash) || hash != z.lay.schemaHash || key < kFrom || key > kTo) continue;
    char path[80];
    snprintf(path, sizeof path, "%s/%s", dir, n.c_str());
    File f = fs.open(path, "r");
    hist::SegHeader h;
    if (!f || !readHeader(f, h) || h.recSize != rs) continue;
    while (f.read(rec, rs) == rs) {
      uint32_t t;
      if (!hist::decodeRecord(def_, z.lay, rec, t, v)) break;  // a torn tail: stop here
      lastOnFile = t;
      if (t < from) continue;
      if (t > to) return;
      if (!fn(ctx, t, v)) return;
    }
  }
  // Records still waiting in RAM, one at a time under the lock.
  for (uint32_t at = 0;; at += rs) {
    {
      Lock l(mx_);
      if (at + rs > z.qLen) return;
      memcpy(rec, z.q + (z.qHead + at) % z.qCap, rs);
    }
    uint32_t t;
    if (!hist::decodeRecord(def_, z.lay, rec, t, v) || t <= lastOnFile || t < from) continue;
    if (t > to) return;
    if (!fn(ctx, t, v)) return;
  }
}

}  // namespace histmod
}  // namespace ESPRack
