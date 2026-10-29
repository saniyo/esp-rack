#include <HistoryService.h>

#include <App.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <ESPFS.h>
#include <ProblemJson.h>
#include <SecurityManager.h>
#include <UpdateError.h>
#include <WebManager.h>
#include <esp_heap_caps.h>
#include <power/PowerGuard.h>
#include <power/WriteGate.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <memory>
#include <new>

#include "HistSeriesImpl.h"

using ESPRack::HistSeriesDef;
using ESPRack::IHistorySeries;
using ESPRack::histmod::Series;

namespace {

constexpr const char* kId = "history";
constexpr const char* kPath = "/config/history.json";
constexpr const char* kRestBase = "/api/v1/history/series";  // /form, /state: the page
constexpr size_t kDocSmall = 4096;      // one page without PSRAM, like DataApi
constexpr size_t kDocLarge = 16384;     // with PSRAM (the malloc policy puts it there)
constexpr size_t kSlot = 16;            // an ArduinoJson value on the ESP32
constexpr size_t kPageOverhead = 768;   // a rows page's keys, cols and arrays
constexpr size_t kScratch = 4096;       // the task's flush buffer
constexpr uint32_t kRetentionMs = 3600UL * 1000UL;
constexpr uint32_t kMinValidT = 1577836800;
constexpr double kMaxCount = 4294967295.0;

void readConfig(HistoryState& s, JsonObject& root) {
  root["enabled"] = s.set.enabled;
  root["flush_min"] = s.set.flushMin;
  root["budget_kb"] = s.set.budgetKb;
  root["cap_kb"] = s.set.capKb;
  root["heap_series"] = s.set.heapSeries;
}

StateUpdateResult updateSettings(JsonObject& root, HistorySettings& c) {
  JsonObject src = root;
  if (root.containsKey("settings") && root["settings"].is<JsonObject>()) src = root["settings"].as<JsonObject>();
  bool changed = false;
  changed |= FormBuilder::updateValue(src, "enabled", c.enabled);
  changed |= FormBuilder::updateValue(src, "flush_min", c.flushMin);
  changed |= FormBuilder::updateValue(src, "budget_kb", c.budgetKb);
  changed |= FormBuilder::updateValue(src, "cap_kb", c.capKb);
  changed |= FormBuilder::updateValue(src, "heap_series", c.heapSeries);
  return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
}

struct FormRows {
  web::Form& f;
  void text(const char* k, const char* l, const char* i, const char* v, const char* c) {
    web::Text t(k, v);
    t.readOnly().label(l).icon(i);
    if (c) t.color(c);
    f.add(t);
  }
  void num(const char* k, const char* l, const char* i, double v) {
    f.add(web::Number(k, v).readOnly().min(0).max(kMaxCount).format("0").label(l).icon(i));
  }
};

struct JsonRows {
  JsonObject& r;
  void text(const char* k, const char*, const char*, const char* v, const char*) { r[k] = String(v); }
  void num(const char* k, const char*, const char*, double v) { r[k] = v; }
};

// A selected column of a query: col, and an element (-1 = all of it).
struct Pick {
  uint8_t col;
  int element;
};

// A page of rows being built for GET /api/v1/history/<id>.
struct Page {
  const HistSeriesDef* def;
  const Pick* picks;
  size_t nPicks;
  const uint16_t* offsets;  // first value index of each column
  JsonArray rows;
  JsonObject root;
  size_t capacity;          // the response doc's
  size_t rowBytes;          // what one row takes, at most
  bool full = false;
  uint32_t next = 0;
};

void putNumber(JsonArray a, double v) {
  if (isnan(v)) a.add(nullptr);  // unknown is not a number
  else a.add(v);
}

void emitRow(void* ctx, uint32_t t, const double* v, uint16_t) {
  Page* p = static_cast<Page*>(ctx);
  if (p->full) return;
  if (p->root.memoryUsage() + p->rowBytes + 64 > p->capacity) {
    p->full = true;
    p->next = t;
    return;
  }
  JsonArray row = p->rows.createNestedArray();
  row.add(t);
  for (size_t i = 0; i < p->nPicks; ++i) {
    const ESPRack::HistColumn& c = p->def->cols[p->picks[i].col];
    const double* cv = v + p->offsets[p->picks[i].col];
    if (p->picks[i].element >= 0) {
      putNumber(row, cv[p->picks[i].element]);
    } else if (p->picks[i].element <= ESPRack::hist::kRowMin) {
      putNumber(row, ESPRack::hist::rowDerived(cv, c.width, p->picks[i].element));
    } else if (c.width == 1) {
      putNumber(row, cv[0]);
    } else {
      JsonArray a = row.createNestedArray();
      for (uint16_t e = 0; e < c.width; ++e) putNumber(a, cv[e]);
    }
  }
}

// Rows straight from the tier, or through a downsampler.
struct Reader {
  Page* page;
  ESPRack::hist::Downsampler* ds;
};

bool onRow(void* ctx, uint32_t t, const double* v) {
  Reader* r = static_cast<Reader*>(ctx);
  if (r->ds) r->ds->feed(t, v);
  else emitRow(r->page, t, v, 0);
  return !r->page->full;
}

// cols=a,b[5],c[min]: the picks, or every column when absent. false: an
// unknown column (the 400 is sent).
bool parseCols(AsyncWebServerRequest* r, const HistSeriesDef& def, std::vector<Pick>& picks) {
  if (!r->hasParam("cols")) {
    for (uint8_t i = 0; i < def.nCols; ++i) picks.push_back({i, -1});
    return true;
  }
  const String spec = r->getParam("cols")->value();
  int start = 0;
  while (start <= int(spec.length())) {
    int comma = spec.indexOf(',', start);
    if (comma < 0) comma = spec.length();
    Pick p;
    if (comma > start && ESPRack::hist::findColumn(def, spec.c_str() + start, size_t(comma - start), p.col, p.element)) {
      picks.push_back(p);
    } else if (comma > start) {
      sendProblem(r, 400, "bad_request", "Unknown column in cols");
      return false;
    }
    start = comma + 1;
  }
  return true;
}

// GET .../<id>?format=csv: the same rows as CSV, streamed in chunks (no page
// limit). Each chunk reads on from where the last one stopped; a row that
// doesn't fit waits in `pending`. A whole width-N column becomes N columns
// key[0]...key[N-1]; an unknown value is an empty field.
struct CsvJob {
  Series* s = nullptr;
  const HistSeriesDef* def = nullptr;
  uint8_t tier = 0;
  uint32_t cursor = 0, to = 0, res = 0;
  bool downsample = false;
  std::vector<Pick> picks;
  std::vector<uint16_t> offsets;
  std::vector<double> v, acc, out;
  std::vector<uint32_t> cnt;
  std::vector<uint8_t> rec;
  std::unique_ptr<ESPRack::hist::Downsampler> ds;
  String pending;
  size_t pos = 0;
  size_t room = 0;        // what the current chunk can still take
  bool stop = false;
  bool finished = false;  // every row is in `pending` or sent

  void header() {
    pending = "t,time";
    for (const Pick& p : picks) {
      const ESPRack::HistColumn& c = def->cols[p.col];
      char k[48];
      if (p.element >= 0) {
        snprintf(k, sizeof k, ",%s[%d]", c.key, p.element);
        pending += k;
      } else if (p.element <= ESPRack::hist::kRowMin) {
        snprintf(k, sizeof k, ",%s[%s]", c.key,
                 p.element == ESPRack::hist::kRowMin ? "min" : p.element == ESPRack::hist::kRowAvg ? "avg" : "max");
        pending += k;
      } else if (c.width == 1) {
        pending += ',';
        pending += c.key;
      } else {
        for (uint16_t e = 0; e < c.width; ++e) {
          snprintf(k, sizeof k, ",%s[%u]", c.key, unsigned(e));
          pending += k;
        }
      }
    }
    pending += "\r\n";
  }

  static void num(String& line, double x) {
    line += ',';
    if (isnan(x)) return;
    char b[24];
    snprintf(b, sizeof b, "%.7g", x);
    line += b;
  }

  void row(uint32_t t, const double* vals) {
    char b[32];
    snprintf(b, sizeof b, "%lu,", (unsigned long)t);
    pending += b;
    const time_t tt = time_t(t);
    struct tm lt;
    localtime_r(&tt, &lt);
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &lt);
    pending += b;
    for (const Pick& p : picks) {
      const ESPRack::HistColumn& c = def->cols[p.col];
      const double* cv = vals + offsets[p.col];
      if (p.element >= 0) {
        num(pending, cv[p.element]);
      } else if (p.element <= ESPRack::hist::kRowMin) {
        num(pending, ESPRack::hist::rowDerived(cv, c.width, p.element));
      } else {
        for (uint16_t e = 0; e < c.width; ++e) num(pending, cv[e]);
      }
    }
    pending += "\r\n";
    // The next chunk reads on after this row (a group: after its bucket;
    // res 0 is one group for the whole period).
    cursor = !downsample ? t + 1 : res ? t + res : 0xFFFFFFFF;
    if (pending.length() - pos >= room) stop = true;
  }

  static void onGroup(void* ctx, uint32_t t, const double* vals, uint16_t) {
    CsvJob* j = static_cast<CsvJob*>(ctx);
    if (!j->stop) j->row(t, vals);
  }
  static bool onRow(void* ctx, uint32_t t, const double* vals) {
    CsvJob* j = static_cast<CsvJob*>(ctx);
    if (j->ds) j->ds->feed(t, vals);
    else j->row(t, vals);
    return !j->stop;
  }

  size_t fill(uint8_t* buf, size_t maxLen) {
    size_t n = 0;
    auto drain = [&]() {
      const size_t left = pending.length() - pos;
      const size_t k = maxLen - n < left ? maxLen - n : left;
      memcpy(buf + n, pending.c_str() + pos, k);
      n += k;
      pos += k;
      if (pos == pending.length()) {
        pending = "";
        pos = 0;
      }
    };
    drain();
    while (n < maxLen && !finished) {
      room = maxLen - n;
      stop = false;
      if (cursor > to) {
        finished = true;
      } else {
        if (downsample) {
          ds.reset(new (std::nothrow) ESPRack::hist::Downsampler());
          if (!ds) return n;  // no memory: end the file here
          ds->attach(*def, res, acc.data(), cnt.data(), out.data(), s->nValues(tier), onGroup, this,
                    &ESPRack::histmod::utcOffsetAt);
        }
        s->read(ESPFS, tier, cursor, to, v.data(), rec.data(), onRow, this);
        if (!stop) {
          if (ds) ds->finish();
          finished = true;
        }
        ds.reset();
      }
      drain();
    }
    return n;
  }
};

void sendCsv(AsyncWebServerRequest* r, Series* s, uint32_t from, uint32_t to, bool hasRes, uint32_t res,
             uint8_t tier) {
  std::shared_ptr<CsvJob> j(new (std::nothrow) CsvJob());
  if (!j) {
    sendProblem(r, 503, "heap_pressure", "No memory for the export", 1);
    return;
  }
  const HistSeriesDef& def = s->def();
  if (!parseCols(r, def, j->picks)) return;
  j->s = s;
  j->def = &def;
  j->tier = tier;
  j->cursor = from;
  j->to = to;
  j->res = res;
  j->downsample = hasRes && (res == 0 || res > def.tiers[tier].bucketS);
  j->offsets.resize(def.nCols);
  for (uint8_t i = 0, at = 0; i < def.nCols; at += def.cols[i].width, ++i) j->offsets[i] = at;
  const uint16_t n = s->nValues(tier);
  j->v.resize(n);
  j->rec.resize(4 + 8 * n + 2);
  if (j->downsample) {
    j->acc.resize(n);
    j->out.resize(n);
    j->cnt.resize(n);
  }
  j->header();
  AsyncWebServerResponse* resp = r->beginChunkedResponse(
      "text/csv", [j](uint8_t* buf, size_t maxLen, size_t) -> size_t { return j->fill(buf, maxLen); });
  char cd[96];
  snprintf(cd, sizeof cd, "attachment; filename=\"%s.csv\"", def.id);
  resp->addHeader("Content-Disposition", cd);
  r->send(resp);
}

}  // namespace

HistoryService::HistoryService(ESPRack::App* app, AsyncWebServer* server, SecurityManager* sm,
                               ConfigManager* cfgMgr)
    : StatefulService<HistoryState>(),
      _app(app),
      _server(server),
      _sm(sm),
      _mx(xSemaphoreCreateMutex()),
      _psram(ESP.getPsramSize() > 0),
      _cfg(cfgMgr, kId, kPath, 512, this, readConfig,
           [this](JsonObject& root, HistoryState& s) { return loadConfig(root, s); }, false, nullptr,
           [this](HistoryState& s, JsonObject& root) { buildForm(s, root); }) {
  addUpdateHandler([this](const String& origin) { _cfg.saveIfChanged(origin); }, false);
}

HistoryService::~HistoryService() {
  end();
  vSemaphoreDelete(_mx);
}

StateUpdateResult HistoryService::loadConfig(JsonObject& root, HistoryState& s) {
  if (root.isNull() || root.size() == 0) {
    s.set = HistorySettings();
    return StateUpdateResult::CHANGED;
  }
  const StateUpdateResult r = updateSettings(root, s.set);
  if (s.set.flushMin < 1 || s.set.flushMin > 240) s.set.flushMin = FACTORY_HISTORY_FLUSH_MIN;
  if (s.set.budgetKb < 16) s.set.budgetKb = FACTORY_HISTORY_BUDGET_KB;
  if (s.set.capKb < 0) s.set.capKb = FACTORY_HISTORY_CAP_KB;
  return r;
}

StateUpdateResult HistoryService::updateChecked(JsonObject& root, HistoryState& s) {
  HistorySettings next = s.set;
  const StateUpdateResult r = updateSettings(root, next);
  if (r != StateUpdateResult::CHANGED) return r;
  const char* why = nullptr;
  if (next.flushMin < 1 || next.flushMin > 240) why = "flush interval must be 1..240 min";
  else if (next.budgetKb < 16 || next.budgetKb > 65536) why = "write budget must be 16..65536 KB a day";
  else if (next.capKb < 0 || uint64_t(next.capKb) * 1024 > ESPFS.totalBytes()) why = "the cap is larger than the flash volume";
  if (why) {
    ESPRack::setUpdateError(why);
    return StateUpdateResult::ERROR;
  }
  s.set = next;
  return StateUpdateResult::CHANGED;
}

// ===================== IHistoryProvider =====================

Series* HistoryService::findLocked(const char* id) const {
  for (const auto& s : _series) {
    if (strcmp(s->id(), id) == 0) return s.get();
  }
  return nullptr;
}

IHistorySeries* HistoryService::find(const char* id) {
  if (!id) return nullptr;
  Lock l(_mx);
  return findLocked(id);
}

IHistorySeries* HistoryService::series(const HistSeriesDef& def, const char** why) {
  const char* w = nullptr;
  if (!ESPRack::hist::validate(def, &w)) {
    if (why) *why = w;
    return nullptr;
  }
  HistorySettings set;
  read([&](HistoryState& s) { set = s.set; });
  Lock l(_mx);
  if (Series* old = findLocked(def.id)) {
    bool same = old->def().nTiers == def.nTiers;
    for (uint8_t k = 0; same && k < def.nTiers; ++k) {
      ESPRack::hist::Layout lay;
      same = ESPRack::hist::layoutOf(def, k, lay) && lay.schemaHash == old->schemaHash(k) &&
             def.tiers[k].keepS == old->def().tiers[k].keepS;
    }
    if (same) return old;
    // A new schema: the old object stays alive (callers may still hold it)
    // and keeps flushing what it queued; the new one writes new files.
    for (auto it = _series.begin(); it != _series.end(); ++it) {
      if (it->get() == old) {
        _retired.push_back(std::move(*it));
        _series.erase(it);
        break;
      }
    }
  }
  if (_series.size() >= ESPRACK_HISTORY_MAX_SERIES) {
    if (why) *why = "too many series";
    return nullptr;
  }
  std::unique_ptr<Series> s(new Series());
  if (!s->init(def, _psram, uint32_t(set.flushMin) * 60, why)) return nullptr;
  if (!_psram && _internalBytes + s->ramBytes() > ESPRACK_HISTORY_INTERNAL_BYTES) {
    if (why) *why = "no RAM left for its buffers (no PSRAM)";
    return nullptr;
  }
  if (!_psram) _internalBytes += s->ramBytes();
  Series* raw = s.get();
  _series.push_back(std::move(s));
  if (_task) xTaskNotifyGive(_task);  // mount it now
  return raw;
}

// ===================== the "hist" task =====================

namespace {

// "sys.heap": the lowest free / largest block per bucket (a leak shows as a
// slope, a fragment as a falling largest block), the highest client count.
const ESPRack::HistColumn kHeapCols[] = {
    {"int_free", "B", ESPRack::HistAgg::Min, 1, ESPRack::HistType::U32, 1},
    {"int_largest", "B", ESPRack::HistAgg::Min, 1, ESPRack::HistType::U32, 1},
    {"int_min", "B", ESPRack::HistAgg::Min, 1, ESPRack::HistType::U32, 1},
    {"psram_free", "B", ESPRack::HistAgg::Min, 1, ESPRack::HistType::U32, 1},
    {"ws_clients", "", ESPRack::HistAgg::Max, 1, ESPRack::HistType::U16, 1},
};
const ESPRack::HistTier kHeapTiers[] = {{60, 3 * 86400}, {900, 30 * 86400}};
const ESPRack::HistSeriesDef kHeapDef{"sys.heap", "Device memory", kHeapCols, 5, kHeapTiers, 2};

}  // namespace

void HistoryService::sampleHeap() {
  if (!_heap) {
    const char* why = nullptr;
    _heap = series(kHeapDef, &why);
    if (!_heap) {
      log_w("[hist] sys.heap not recorded: %s", why ? why : "?");
      return;
    }
  }
  const time_t now = time(nullptr);
  if (now <= kMinValidT) return;  // no valid time yet: the series takes none
  constexpr uint32_t kInt = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  WsManager* ws = _app ? _app->wsManager() : nullptr;
  const double v[5] = {
      double(heap_caps_get_free_size(kInt)),
      double(heap_caps_get_largest_free_block(kInt)),
      double(heap_caps_get_minimum_free_size(kInt)),
      double(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
      double(ws ? ws->totalClients() : 0),
  };
  (void)_heap->add(uint32_t(now), v, 5);
}

namespace {

// A slot of the flash write gate (power/WriteGate.h) per step of a series'
// mount / retention / erase, given back as soon as the step's write is
// over. One wait clock per operation (finish() ends it), so maxWaitMs
// counts across its steps. Stopping: at once. take() false: PowerGuard
// paused writes.
class GateSteps {
 public:
  explicit GateSteps(const std::atomic<bool>* stop) : _stop(stop) {}
  ~GateSteps() { finish(); }
  bool take() {
    release();
    if (!_since) _since = ESPRack::WriteGate::now();
    _held = ESPRack::WriteGate::enter(0, _stop, _since, &_at);
    return _held;
  }
  void release() {
    if (_held) ESPRack::WriteGate::leave(_at);
    _held = false;
  }
  void finish() {
    release();
    _since = 0;
  }
  Series::Step step() { return {[this] { return take(); }, [this] { release(); }}; }

 private:
  const std::atomic<bool>* _stop;
  bool _held = false;
  uint32_t _at = 0;
  uint32_t _since = 0;
};

}  // namespace

void HistoryService::begin() {
  (void)_cfg.ensureLoaded();
  if (_server) {
    auto get = [this](AsyncWebServerRequest* r) { handleRest(r); };
    auto del = [this](AsyncWebServerRequest* r) { handleRest(r); };
    _server->on(kRestBase, HTTP_GET,
                _sm ? _sm->wrapRequest(get, AuthenticationPredicates::IS_AUTHENTICATED) : ArRequestHandlerFunction(get));
    _server->on(kRestBase, HTTP_DELETE,
                _sm ? _sm->wrapRequest(del, AuthenticationPredicates::IS_ADMIN) : ArRequestHandlerFunction(del));
  }
  _stop = false;
  xTaskCreatePinnedToCore(taskEntry, "hist", 6144, this, 1, &_task, tskNO_AFFINITY);
}

void HistoryService::end() {
  if (!_task) return;
  _stop = true;
  ESPRack::WriteGate::nudge();  // a step waiting for the write gate goes now
  xTaskNotifyGive(_task);
  for (int i = 0; i < 300 && _running; ++i) vTaskDelay(pdMS_TO_TICKS(10));  // the task flushes, then leaves
  _task = nullptr;
}

void HistoryService::taskEntry(void* self) {
  static_cast<HistoryService*>(self)->run();
  vTaskDelete(nullptr);
}

uint32_t HistoryService::capBytes() {
  HistorySettings set;
  read([&](HistoryState& s) { set = s.set; });
  return set.capKb ? uint32_t(set.capKb) * 1024 : uint32_t(ESPFS.totalBytes() / 4);
}

void HistoryService::flushAll(bool force) {
  if (!ESPRack::PowerGuard::writesAllowed()) {
    ++_skippedPower;
    return;
  }
  uint8_t* scratch = static_cast<uint8_t*>(
      _psram ? heap_caps_malloc(kScratch, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : malloc(kScratch));
  if (!scratch) return;
  std::vector<Series*> list;
  {
    Lock l(_mx);
    for (auto& s : _series) list.push_back(s.get());
    for (auto& s : _retired) list.push_back(s.get());
  }
  size_t written = 0;
  bool paused = false;
  // One wait clock for the pass, so maxWaitMs bounds it, not each chunk.
  const uint32_t passSince = ESPRack::WriteGate::now();
  for (Series* s : list) {
    if (!s->mounted() || s->erasing() || (!force && !s->queuedBytes())) continue;
    // A slot of the flash write gate (power/WriteGate.h) per chunk of up to
    // kScratch bytes (the tiers take turns): a pause between a module's
    // windows takes one write. What is queued now, not what comes meanwhile:
    // the pass ends. Stopping: at once.
    uint32_t left = s->queuedBytes();
    while (left) {
      ESPRack::WriteSlot slot(0, &_stop, passSince);
      if (!slot.ok()) {  // PowerGuard paused writes: the rest stays queued
        ++_skippedPower;
        paused = true;
        break;
      }
      const size_t w = s->flush(ESPFS, scratch, kScratch, 1);
      if (!w) break;
      written += w;
      left = w >= left ? 0 : left - uint32_t(w);
    }
    if (paused) break;
  }
  free(scratch);
  _lastFlushMs = millis();
  if (!written) return;
  ++_flushes;
  const time_t now = time(nullptr);
  if (now > kMinValidT) _lastFlushT = uint32_t(now);
  _bytesToday += uint32_t(written);
  HistorySettings set;
  read([&](HistoryState& s) { set = s.set; });
  if (_bytesToday > uint32_t(set.budgetKb) * 1024 && _stretch < 16) {
    _stretch = _stretch * 2;
    log_w("[hist] %u KB written today (budget %d KB): flushing every %u min", unsigned(_bytesToday / 1024),
          set.budgetKb, unsigned(set.flushMin * _stretch));
  }
}

void HistoryService::run() {
  _running = true;
  _lastFlushMs = millis();
  _lastRetentionMs = millis();
  while (!_stop) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    if (_stop) break;
    std::vector<Series*> list;
    std::vector<String> erase;
    {
      Lock l(_mx);
      for (auto& s : _series) list.push_back(s.get());
      erase.swap(_eraseIds);
    }
    // Mounting reads (a gate slot only to cut a torn tail); erasing removes
    // files, a slot each. An erase a refused step left goes again next pass.
    GateSteps steps(&_stop);
    for (Series* s : list) {
      if (!s->mounted()) (void)s->mount(ESPFS, steps.step());
      steps.finish();
    }
    for (const String& id : erase) {
      for (Series* s : list) {
        if (!id.isEmpty() && id != s->id()) continue;
        const bool done = s->erase(ESPFS, steps.step());
        steps.finish();
        if (!done) {  // the rest of this series' files next pass (not the series done already)
          Lock l(_mx);
          _eraseIds.push_back(String(s->id()));
        }
      }
    }
    const time_t now = time(nullptr);
    if (now > kMinValidT && uint32_t(now / 86400) != _dayKey) {
      _dayKey = uint32_t(now / 86400);
      _bytesToday = 0;
      _stretch = 1;
    }
    HistorySettings set;
    read([&](HistoryState& s) { set = s.set; });
    if (set.heapSeries && millis() - _lastHeapMs >= ESPRACK_HISTORY_HEAP_SAMPLE_MS) {
      _lastHeapMs = millis();
      sampleHeap();
    }
    if (!set.enabled) continue;
    bool due = _flushNow.exchange(false) || millis() - _lastFlushMs >= uint32_t(set.flushMin) * 60000UL * _stretch;
    for (Series* s : list) due = due || s->pressing();
    if (due) flushAll(false);
    if (millis() - _lastRetentionMs >= kRetentionMs && now > kMinValidT) {
      _lastRetentionMs = millis();
      // The global cap goes to the series without their own maxBytes, in
      // proportion to what each needs for its whole retention.
      std::vector<Series*> pooled;
      std::vector<uint64_t> need;
      for (Series* s : list) {
        if (s->def().maxBytes) {
          s->retention(ESPFS, uint32_t(now), s->def().maxBytes, steps.step());
          steps.finish();
        } else {
          pooled.push_back(s);
          need.push_back(s->needBytes());
        }
      }
      std::vector<uint32_t> share(pooled.size());
      ESPRack::hist::splitByNeed(capBytes(), need.data(), pooled.size(), share.data());
      for (size_t i = 0; i < pooled.size(); ++i) {
        pooled[i]->retention(ESPFS, uint32_t(now), share[i], steps.step());
        steps.finish();
      }
    }
  }
  flushAll(true);  // shutdown, OTA, restart: what is waiting goes to flash (if power allows)
  _running = false;
}

// ===================== REST =====================

void HistoryService::handleRest(AsyncWebServerRequest* r) {
  if (ESP.getMaxAllocHeap() < kDocSmall + 1024 && !_psram) {
    sendProblem(r, 503, "heap_pressure", "History response needs more contiguous heap than is free", 1);
    return;
  }
  String rest = r->url().substring(strlen(kRestBase));
  if (rest.length() <= 1) {
    if (r->method() == HTTP_GET) sendList(r);
    else sendProblem(r, 405, "method_not_allowed", "Use DELETE /api/v1/history/<id>");
    return;
  }
  const String id = rest.substring(1);
  Series* s = nullptr;
  {
    Lock l(_mx);
    s = findLocked(id.c_str());
  }
  if (!s) {
    sendProblem(r, 404, "not_found", "No such history series");
    return;
  }
  if (r->method() == HTTP_DELETE) {
    {
      Lock l(_mx);
      _eraseIds.push_back(id);
    }
    if (_task) xTaskNotifyGive(_task);
    r->send(202, "application/json", "{\"ok\":true}");
    return;
  }
  sendRows(r, s);
}

void HistoryService::sendList(AsyncWebServerRequest* r) {
  const size_t capacity = _psram ? kDocLarge : kDocSmall;
  AsyncJsonResponse* resp = new AsyncJsonResponse(false, capacity);
  JsonObject root = resp->getRoot().to<JsonObject>();
  // The device's clock (the UI plans periods on it, not on the browser's),
  // and how many values one page of rows can hold (a request packs its
  // columns to it).
  const time_t nowT = time(nullptr);
  if (nowT > kMinValidT) root["now"] = uint32_t(nowT);
  root["maxv"] = uint32_t((capacity - kPageOverhead) / kSlot);
  JsonArray arr = root.createNestedArray("series");
  Lock l(_mx);
  for (const auto& sp : _series) {
    const Series& s = *sp;
    const HistSeriesDef& d = s.def();
    const ESPRack::HistSeriesStats st = s.stats();
    JsonObject o = arr.createNestedObject();
    o["id"] = d.id;
    o["title"] = d.title ? d.title : "";
    if (d.localDays) o["local_days"] = true;
    JsonArray cols = o.createNestedArray("cols");
    for (uint8_t i = 0; i < d.nCols; ++i) {
      JsonObject c = cols.createNestedObject();
      c["key"] = d.cols[i].key;
      if (d.cols[i].unit && *d.cols[i].unit) c["unit"] = d.cols[i].unit;
      c["agg"] = ESPRack::histAggName(d.cols[i].agg);
      if (d.cols[i].width > 1) c["width"] = d.cols[i].width;
    }
    JsonArray tiers = o.createNestedArray("tiers");
    for (uint8_t k = 0; k < d.nTiers; ++k) {
      JsonObject t = tiers.createNestedObject();
      t["bucket"] = d.tiers[k].bucketS;
      t["keep"] = d.tiers[k].keepS;
    }
    o["records"] = st.records;
    o["bytes"] = st.bytes;
    if (st.oldBytes) o["old_bytes"] = st.oldBytes;
    if (st.oldestT) o["oldest"] = st.oldestT;
    if (st.newestT) o["newest"] = st.newestT;
    o["queued"] = st.queued;
    if (st.full) o["full"] = st.full;
    if (st.stale) o["stale"] = st.stale;
  }
  resp->setLength();
  r->send(resp);
}

void HistoryService::sendRows(AsyncWebServerRequest* r, Series* s) {
  const HistSeriesDef& def = s->def();
  const time_t nowT = time(nullptr);
  const uint32_t now = nowT > 0 ? uint32_t(nowT) : 0;
  auto param = [r](const char* k, uint32_t dflt) -> uint32_t {
    return r->hasParam(k) ? uint32_t(strtoul(r->getParam(k)->value().c_str(), nullptr, 10)) : dflt;
  };
  const uint32_t from = param("from", now > 86400 ? now - 86400 : 0);
  const uint32_t to = param("to", 0xFFFFFFFF);
  const bool hasRes = r->hasParam("res");
  const uint32_t res = param("res", 0);
  // The tier: tier=<index> when valid (the UI picks one for both periods of
  // a comparison), else from the period's start. Always from `from`, not the
  // cursor, so every page of one answer reads the same tier.
  const uint32_t tierArg = param("tier", 0xFF);
  const uint8_t tier = tierArg < def.nTiers ? uint8_t(tierArg) : s->pickTier(from, now);
  const uint32_t cursor = param("cursor", from);
  const uint32_t start = cursor > from ? cursor : from;

  if (r->hasParam("format") && r->getParam("format")->value() == "csv") {
    sendCsv(r, s, start, to, hasRes, res, tier);
    return;
  }

  // Columns: all, or the listed ones ("a,b[5]").
  std::vector<Pick> picks;
  if (!parseCols(r, def, picks)) return;
  std::vector<uint16_t> offsets(def.nCols);
  for (uint8_t i = 0, at = 0; i < def.nCols; at += def.cols[i].width, ++i) offsets[i] = at;

  const uint16_t n = s->nValues(tier);
  const size_t capacity = _psram ? kDocLarge : kDocSmall;
  AsyncJsonResponse* resp = new AsyncJsonResponse(false, capacity);
  JsonObject root = resp->getRoot().to<JsonObject>();
  if (root.isNull()) {
    delete resp;
    sendProblem(r, 503, "heap_pressure", "Response buffer alloc failed", 1);
    return;
  }
  root["series"] = def.id;
  root["tier"] = tier;
  root["bucket"] = def.tiers[tier].bucketS;
  if (hasRes) root["res"] = res;
  JsonArray cols = root.createNestedArray("cols");
  cols.add("t");
  for (const Pick& p : picks) {
    char k[48];
    if (p.element == ESPRack::hist::kAll) {
      cols.add(def.cols[p.col].key);
      continue;
    }
    if (p.element >= 0) snprintf(k, sizeof k, "%s[%d]", def.cols[p.col].key, p.element);
    else snprintf(k, sizeof k, "%s[%s]", def.cols[p.col].key,
                  p.element == ESPRack::hist::kRowMin ? "min" : p.element == ESPRack::hist::kRowAvg ? "avg" : "max");
    cols.add(String(k));
  }

  size_t slots = 2;  // the row array and t
  for (const Pick& p : picks) slots += (p.element != ESPRack::hist::kAll || def.cols[p.col].width == 1) ? 1 : def.cols[p.col].width + 1;
  Page page{&def, picks.data(), picks.size(), offsets.data(), root.createNestedArray("rows"), root, capacity,
            slots * kSlot};
  std::vector<double> v(n), acc, out;
  std::vector<uint32_t> cnt;
  std::vector<uint8_t> rec(4 + 8 * n + 2);  // the widest record a tier can have
  ESPRack::hist::Downsampler ds;
  Reader reader{&page, nullptr};
  if (hasRes && (res == 0 || res > def.tiers[tier].bucketS)) {
    acc.resize(n);
    out.resize(n);
    cnt.resize(n);
    ds.attach(def, res, acc.data(), cnt.data(), out.data(), n, emitRow, &page,
              &ESPRack::histmod::utcOffsetAt);  // whole days: local midnights
    reader.ds = &ds;
  }
  s->read(ESPFS, tier, start, to, v.data(), rec.data(), onRow, &reader);
  if (reader.ds) ds.finish();
  if (page.full) root["next"] = page.next;
  resp->setLength();
  r->send(resp);
}

// ===================== page =====================

namespace {

template <class E>
void statusRows(const HistoryState& s, uint32_t lastFlushT, uint32_t bytesToday, uint32_t stretch, uint32_t flushes,
                uint32_t skipped, E& e) {
  char buf[96];
  const uint64_t total = ESPFS.totalBytes(), used = ESPFS.usedBytes();
  e.text("writer", "Writing", "Save",
         !s.set.enabled ? "off (kept in RAM only)"
                        : (!ESPRack::PowerGuard::writesAllowed() ? "paused: power is failing" : "on"),
         s.set.enabled ? "success" : "warning");
  snprintf(buf, sizeof buf, "flash: %lu of %lu KB used", (unsigned long)(used / 1024), (unsigned long)(total / 1024));
  e.text("volume", "Storage", "Storage", buf, nullptr);
  snprintf(buf, sizeof buf, "%lu KB of %d KB budget%s", (unsigned long)(bytesToday / 1024), s.set.budgetKb,
           stretch > 1 ? " — over budget: flushing less often" : "");
  e.text("today", "Written today", "DataUsage", buf, stretch > 1 ? "warning" : nullptr);
  // Rough wear: LittleFS levels writes over the volume; ~100k erase cycles,
  // x2 for metadata. Projected from today's rate.
  const time_t now = time(nullptr);
  const uint32_t secs = now > kMinValidT ? uint32_t(now % 86400) + 1 : 0;
  if (secs > 600 && bytesToday) {
    const double perDay = double(bytesToday) * 86400.0 / secs;
    const double years = double(total) * 100000.0 / (perDay * 2.0) / 365.0;
    snprintf(buf, sizeof buf, years > 100 ? "over 100 years at today's rate" : "about %.0f years at today's rate", years);
    e.text("wear", "Flash wear", "Timer", buf, years < 10 ? "warning" : nullptr);
  }
  if (lastFlushT) {
    snprintf(buf, sizeof buf, "%lu s ago (every %d min)", (unsigned long)(now - lastFlushT), int(s.set.flushMin * stretch));
  } else {
    snprintf(buf, sizeof buf, "not yet (every %d min)", int(s.set.flushMin * stretch));
  }
  e.text("last_flush", "Last write to flash", "History", buf, nullptr);
  e.num("flushes", "Writes since boot", "Save", flushes);
  if (skipped) e.num("skipped_power", "Writes held back (power failing)", "Warning", skipped);
}

}  // namespace

void HistoryService::buildForm(HistoryState& s, JsonObject& root) {
  web::Form sf(root, "status", "History");
  FormRows rows{sf};
  statusRows(s, _lastFlushT, _bytesToday, _stretch, _flushes, _skippedPower, rows);
  sf.add(web::Table("series_table")
             .col("id", "Series", "text")
             .col("records", "Records", "number", "0")
             .col("kb", "KB", "number", "0")
             .col("old_kb", "Old schema KB", "number", "0")
             .col("queued", "Waiting", "number", "0")
             .col("full", "Refused (full)", "number", "0")
             .col("stale", "Refused (old time)", "number", "0")
             .mode("replace")
             .label("Series")
             .icon("TableChart"));
  JsonArray tr = sf.rowsOf("series_table");
  {
    Lock l(_mx);
    for (const auto& sp : _series) {
      const ESPRack::HistSeriesStats st = sp->stats();
      JsonObject o = tr.createNestedObject();
      o["id"] = String(sp->id());
      o["records"] = st.records;
      o["kb"] = (st.bytes + 1023) / 1024;
      o["old_kb"] = (st.oldBytes + 1023) / 1024;
      o["queued"] = st.queued;
      o["full"] = st.full;
      o["stale"] = st.stale;
    }
  }
  sf.add(web::Action("a_flush").ref("history.flush"));
  sf.add(web::Action("a_erase").ref("history.eraseAll"));

  const HistorySettings& c = s.set;
  web::Form set(root, "settings", "History settings");
  set.add(web::Switch("enabled", c.enabled).label("Write to flash").icon("Save"));
  set.add(web::Number("flush_min", c.flushMin).min(1).max(240).format("0").unit("min")
              .label("Write what is waiting every").icon("Timer"));
  set.add(web::Number("budget_kb", c.budgetKb).min(16).max(65536).format("0").unit("KB")
              .label("Write budget a day (past it, writes get rarer)").icon("DataUsage"));
  set.add(web::Number("cap_kb", c.capKb).min(0).format("0").unit("KB")
              .label("Space for all history (0 = a quarter of the flash)").icon("Storage"));
  set.add(web::Switch("heap_series", c.heapSeries).label("Record the device's memory (series sys.heap)")
              .icon("Memory"));
  set.add(web::Message("m_hist", "History is kept in RAM and written to flash in batches, so the flash isn't worn by "
                                 "every sample. An unexpected reset loses what was waiting for the next write.")
              .level("info"));
}

void HistoryService::readState(HistoryState& s, JsonObject& root) {
  JsonRows rows{root};
  statusRows(s, _lastFlushT, _bytesToday, _stretch, _flushes, _skippedPower, rows);
  readConfig(s, root);
}

void HistoryService::registerManifest(WebManager* web) {
  if (!web) return;
  auto checked = [this](JsonObject& root, HistoryState& s) { return updateChecked(root, s); };
  auto form = [this](HistoryState& s, JsonObject& root) { buildForm(s, root); };
  auto state = [this](HistoryState& s, JsonObject& root) { readState(s, root); };
  _feature = web->registerFeature<HistoryState>(
      WebFeature(kId)
          .title("History")
          .version(HISTORY_MODULE_VERSION)
          .component(WebComponent::DynamicSettings)
          .authenticated()
          .menu(WebMenu("History").icon("History").order(150).expandTabs(false))
          .tab(WebTab("status", "Status").icon("Assessment"))
          .tab(WebTab("settings", "Settings").icon("Settings").postable().adminWrite())
          .build(),
      this, form, checked, state, checked, WEBFEATURE_BUFFER_AUTO, 2048);

  web->registerAction(WebAction("history.flush", "Write now")
                          .icon("Save")
                          .color("primary")
                          .success("Writing")
                          .handler([this](AsyncWebServerRequest* r) {
                            _flushNow = true;
                            if (_task) xTaskNotifyGive(_task);
                            r->send(200, "application/json", "{\"ok\":true}");
                          }));
  web->registerAction(WebAction("history.eraseAll", "Delete all history")
                          .icon("Delete")
                          .color("error")
                          .confirm("Delete every series' history from flash?")
                          .success("Deleting")
                          .handler([this](AsyncWebServerRequest* r) {
                            {
                              Lock l(_mx);
                              _eraseIds.push_back(String());
                            }
                            if (_task) xTaskNotifyGive(_task);
                            r->send(200, "application/json", "{\"ok\":true}");
                          }));
}
