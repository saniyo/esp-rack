#pragma once
#ifndef ESPRACK_HISTORY_SERVICE_H
#define ESPRACK_HISTORY_SERVICE_H

// HistoryService — the working half of HistoryModule (docs/plans/history.md):
// the IHistoryProvider other modules register series with, the "hist" task
// that writes them to flash in batches, the REST queries and the page.
//
//   * Series live in RAM (PSRAM when there is some) until the task flushes
//     them: every flush_min minutes, when a series' buffer fills up, and on
//     shutdown — only while PowerGuard allows writes. An unexpected reset
//     loses at most what was waiting and the open buckets.
//   * Retention runs once an hour: by each tier's time, then by size (the
//     series' maxBytes, else a share of the global cap in proportion to
//     what its retention needs; within a series, per tier the same way).
//   * Writes are counted per day against budget_kb_day; past it the flush
//     interval doubles (up to 16x) and the page says so, with a rough flash
//     wear estimate.
//   * GET /api/v1/history/series                the series, the device's
//                                              "now" and "maxv" (values one
//                                              page of rows can hold)
//     GET /api/v1/history/series/<id>?from&to&res&cols&tier&cursor
//                                              rows, ascending, one page (4 KB,
//                                              16 KB with PSRAM); res 0 = the
//                                              whole period in one row;
//                                              tier=<index> reads that tier
//                                              (else the finest covering
//                                              from); &format=csv: all of it
//                                              as a streamed CSV file
//     DELETE /api/v1/history/series/<id>       erase a series' files (admin)
//     (/api/v1/history/form and /state are the page's.)

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>
#include <vector>

#include <ConfigDelegate.h>
#include <FormBuilder.h>
#include <StatefulService.h>
#include <WebFeatureDelegate.h>
#include <history/IHistory.h>
#include <web/Web.h>

class AsyncWebServer;
class AsyncWebServerRequest;
class SecurityManager;
class WebManager;

namespace ESPRack {
class App;
namespace histmod {
class Series;
}
}  // namespace ESPRack

#define HISTORY_MODULE_VERSION "0.1.1"

#ifndef FACTORY_HISTORY_FLUSH_MIN
#define FACTORY_HISTORY_FLUSH_MIN 15
#endif
#ifndef FACTORY_HISTORY_BUDGET_KB
#define FACTORY_HISTORY_BUDGET_KB 1024  // KB written per day before the interval stretches
#endif
#ifndef FACTORY_HISTORY_CAP_KB
#define FACTORY_HISTORY_CAP_KB 0  // all series together; 0 = a quarter of the flash volume
#endif
// The device's own memory as a series ("sys.heap"): the internal heap (free,
// largest block, lowest since boot), PSRAM free and the WebSocket clients,
// sampled every ESPRACK_HISTORY_HEAP_SAMPLE_MS (the lowest / highest per
// minute kept). For a leak's slope on the chart. A setting; this is its
// factory value.
#ifndef FACTORY_HISTORY_HEAP_SERIES
#define FACTORY_HISTORY_HEAP_SERIES false
#endif
#ifndef ESPRACK_HISTORY_HEAP_SAMPLE_MS
#define ESPRACK_HISTORY_HEAP_SAMPLE_MS 10000
#endif
#ifndef ESPRACK_HISTORY_MAX_SERIES
#define ESPRACK_HISTORY_MAX_SERIES 16
#endif
#ifndef ESPRACK_HISTORY_INTERNAL_BYTES
#define ESPRACK_HISTORY_INTERNAL_BYTES 16384  // buffers in the internal heap when there is no PSRAM
#endif

struct HistorySettings {
  bool enabled = true;
  int flushMin = FACTORY_HISTORY_FLUSH_MIN;
  int budgetKb = FACTORY_HISTORY_BUDGET_KB;
  int capKb = FACTORY_HISTORY_CAP_KB;
  bool heapSeries = FACTORY_HISTORY_HEAP_SERIES;  // record "sys.heap"
};

class HistoryState {
 public:
  HistorySettings set;
};

class HistoryService : public StatefulService<HistoryState>, public ESPRack::IHistoryProvider {
 public:
  HistoryService(ESPRack::App* app, AsyncWebServer* server, SecurityManager* sm, ConfigManager* cfgMgr);
  ~HistoryService();

  void registerManifest(WebManager* web);
  void begin();  // starts the "hist" task
  void end();    // flushes (if allowed) and stops it

  // ---- IHistoryProvider ----
  ESPRack::IHistorySeries* series(const ESPRack::HistSeriesDef& def, const char** why = nullptr) override;
  ESPRack::IHistorySeries* find(const char* id) override;
  bool ready() const override { return _running; }

 private:
  struct Lock {
    explicit Lock(SemaphoreHandle_t m) : _m(m) { xSemaphoreTake(_m, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(_m); }
    SemaphoreHandle_t _m;
  };

  static void taskEntry(void* self);
  void run();
  void flushAll(bool force);
  uint32_t capBytes();
  void handleRest(AsyncWebServerRequest* r);
  void sendList(AsyncWebServerRequest* r);
  void sendRows(AsyncWebServerRequest* r, ESPRack::histmod::Series* s);
  void buildForm(HistoryState& s, JsonObject& root);
  void readState(HistoryState& s, JsonObject& root);
  StateUpdateResult loadConfig(JsonObject& root, HistoryState& s);
  StateUpdateResult updateChecked(JsonObject& root, HistoryState& s);
  ESPRack::histmod::Series* findLocked(const char* id) const;

  ESPRack::App* _app;
  AsyncWebServer* _server;
  SecurityManager* _sm;
  SemaphoreHandle_t _mx;  // _series, _retired, _eraseIds
  std::vector<std::unique_ptr<ESPRack::histmod::Series>> _series;
  std::vector<std::unique_ptr<ESPRack::histmod::Series>> _retired;  // replaced by a new schema: kept alive
  std::vector<String> _eraseIds;  // "" = all
  bool _psram;
  uint32_t _internalBytes{0};
  TaskHandle_t _task{nullptr};
  std::atomic<bool> _running{false};
  std::atomic<bool> _stop{false};
  std::atomic<bool> _flushNow{false};
  WebFeatureEntry<HistoryState>* _feature{nullptr};

  // the "hist" task's view, read by the page
  std::atomic<uint32_t> _lastFlushT{0};
  std::atomic<uint32_t> _bytesToday{0};
  std::atomic<uint32_t> _dayKey{0};
  std::atomic<uint32_t> _stretch{1};  // flush interval multiplier (budget)
  std::atomic<uint32_t> _flushes{0};
  std::atomic<uint32_t> _skippedPower{0};
  uint32_t _lastFlushMs{0};      // the task's own
  uint32_t _lastRetentionMs{0};
  uint32_t _lastHeapMs{0};
  ESPRack::IHistorySeries* _heap{nullptr};  // "sys.heap" once registered (the hist task)
  void sampleHeap();

  // Last member: its constructor already runs buildForm (the secret-key probe),
  // and the form reads the members above.
  ConfigDelegate<HistoryState> _cfg;
};

#endif  // ESPRACK_HISTORY_SERVICE_H
