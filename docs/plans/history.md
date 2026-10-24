# History module (`modules/history`)

This is the agreed design: the owner's request, the JK gateway's needs and
SES-LIB-017 (SES Light), from 2026-10-24.

It is one framework component that keeps time series on flash or an SD card
without wearing the flash out. It serves them for charts, bars, pies and
tables, over a period the page chooses.

## Consumers

The design has to fit these:

- **JK gateway.**
  - Per pack, one series `jkBalance.pN`. It has six Sum columns, each with
    one value per cell (width N, for example 96): `in_mah`, `out_mah`,
    `in_chg`, `out_chg`, `in_dis`, `out_dis`.
  - Tiers: hour, then day.
  - The width is known only after the config is loaded, in `begin()`.
  - Later: inverter SOC, limits and current, and per-cell voltages
    (Min/Max).
- **SES Light.**
  - `months` and `nights` are put-series of fixed records: one record per
    put, with integer fields from 1 to 8 bytes, signed or unsigned. That is
    about 400 records a year.
  - `power` uses 5-minute slots rolled up to hours and days: mean W, plus
    a sum of the seconds the relay was on.
  - `events` has tens of entries a day.

## Core interface (`lib/ESPRack/include/history/IHistory.h`)

```cpp
namespace ESPRack {
enum class HistAgg : uint8_t { Sum, Avg, Min, Max, Last };
enum class HistType : uint8_t { I8, U8, I16, U16, I32, U32, I64, U64, F32 };
enum class HistPut : uint8_t { Ok, Duplicate, Stale, Full, NotReady, BadArgs };
struct HistColumn {
  const char* key; const char* unit; HistAgg agg;
  uint16_t width = 1;                 // > 1: an array (one value per cell)
  HistType type = HistType::F32;
  float scale = 1;                    // stored = value * scale (integer types)
};
struct HistTier { uint32_t bucketS; uint32_t keepS; };  // bucketS 0 = one record per put
struct HistSeriesDef {
  const char* id; const char* title;   // id: lowercase LDH + dots, stable
  const HistColumn* cols; uint8_t nCols;
  const HistTier* tiers; uint8_t nTiers;  // tier 0 + rollups (rollups only when bucketS > 0)
  uint32_t maxBytes = 0;                  // 0 = a share of the global cap
};
class IHistorySeries {
  virtual HistPut add(uint32_t t, const double* v, size_t n) = 0;       // sample -> open bucket (agg)
  virtual HistPut put(uint32_t t, const double* v, size_t n) = 0;       // a finished bucket / record
  virtual HistPut putRaw(uint32_t t, const void* rec, size_t len) = 0;  // put-series: exact integer record
  virtual uint32_t newestT() const = 0;                                 // 0 = empty
  virtual HistSeriesStats stats() const = 0;
};
class IHistoryProvider {
  virtual IHistorySeries* series(const HistSeriesDef&, const char** why = nullptr) = 0;  // any time
  virtual IHistorySeries* find(const char* id) = 0;
  virtual bool ready() const = 0;
};
// App::history() / setHistory(), late-bound like ble().
}
```

### Rules

- **When to register.** `series()` works at any time: in `onInstall`, in
  `begin()` or later. Changing a schema (columns, width, type) gives a new
  schema hash.
  - New segments are written in the new schema.
  - Old segments stay readable, but queries never mix the two schemas.
  - Old segments are dropped by retention. The page shows "N KB in an old
    schema" with a delete button.
- **Types on rollup tiers.** Sum columns are widened, so I16 becomes I32
  with the same scale. Avg, Min, Max and Last keep their type.
- **Put-series.** A put-series (tier 0 with `bucketS = 0`) has a single
  tier and no rollup.
  - The caller sets `t`. The module does not check it against the current
    time.
  - `t ≤ newestT()` gives `Duplicate` or `Stale`, and nothing is written.
    There is no replace.
  - After a reboot, SES re-appends whatever is missing from its NVS records.
- **Full buffer.**
  - `put()` on a full buffer returns `Full`. It never drops data silently
    and never evicts anything.
  - Each series has its own buffer quota. Put-series also get a guaranteed
    reserve.
  - When `add()` overflows, it only counts the overflow. The bucket keeps
    aggregating in RAM.
- **Time.**
  - `add()` without a valid time is refused.
  - After the clock steps back, nothing with `t < newest` reaches the disk.
    It is counted as `Stale`, and the page shows the count.

## Storage

- **Buffers.** Open buckets and closed records waiting for flush live in
  PSRAM. Without PSRAM they live in the internal heap with smaller limits.
  The code is the same.
- **Files.** Segment files are append-only:
  `/hist/<id>/t<tier>/<YYYYMMDD | YYYYMM>.seg`.
  - The header is `{'HSEG', schemaHash, recSize, bucketS, start}`.
  - Each record is `[t u32][values][crc16]`.
  - At mount, a bad tail is cut off, and `newest` comes from the last whole
    record.
  - There are no `.partial` names: LIB-FIX-007 purges those at boot.
- **Writer.** A low-priority task named `hist` writes to the files.
  - It writes in batches.
  - It holds the FS lock for at most about 100 ms per chunk.
  - loopTask only enqueues under a short lock.
- **When it flushes:**
  - every `flush_min` minutes (default 15);
  - when a series' buffer is full;
  - on shutdown or OTA;
  - only while `PowerGuard::writesAllowed()`. Otherwise the buffer stays in
    RAM.
- **What a reset loses.** An unexpected reset loses at most the unflushed
  buffer (up to `flush_min`) and the open buckets. The documentation says
  so.
- **Retention.** Retention works by time (`keepS` per tier) and by size
  (`maxBytes` per series, plus a global cap). The oldest whole segments are
  deleted.
- **Write budget.**
  - The budget is in KB per day. Exceeding it doubles the flush interval
    and shows a warning.
  - The page shows bytes written today and an estimate of flash wear in
    years.
- **Backend.** The backend is flash (LittleFS) or SD, through
  FileSystemModule volumes, chosen in the settings. If there is no SD, the
  module uses flash and shows a note.

## Queries

- `GET /api/v1/history` lists the series: columns, tiers, bytes, oldest and
  newest, and the schema.
- `GET /api/v1/history/<id>?from&to&res&cols=a,b[5]&cursor` returns
  `{cols, rows:[[t, …]], next?}`.
  - Rows come in ascending time.
  - It uses the finest tier whose bucket ≤ `res`, and downsamples by each
    column's aggregate.
  - `res=0` reduces the whole period to one row, for example totals per
    cell.
  - A page stays within the DataApi pool (4096 B). The async_tcp stack is
    6 KB, so nothing large goes on the stack.
- `GET /api/v1/history/<id>.csv?from&to&res` is streamed.

## Form DSL and UI

- `web::Trend("k").history("jkBalance.p1", "in_mah,out_mah").range("7d")`.
  `web::Table("t").history(...)` works the same way.
  - The form only declares the binding: the manifest stays a skeleton.
  - The UI fetches the data from the history REST API, not from `/state`
    or WS.
- A period picker offers 24 h, 7 d, 30 d, all and custom. `.fixedRange()`
  hides it.
- `.reduce()` uses `res=0`: x is the cell, shown as bars or a pie.

## Page "History"

- **Status:** the backend, used and total space, writes today against the
  budget, the wear estimate, the last flush, the records queued, and the
  `full` and `stale` counters.
- **Series:** a table with id, title, tiers, retention, bytes, records,
  oldest and newest, and old-schema bytes.
- **Settings:** backend, `flush_min`, budget and global cap.
- **Actions:** Flush now, Export CSV, Delete series and Delete all, each
  with a confirmation.

## Same on all chips

Without PSRAM or SD, the limits shrink: fewer series, narrower width,
smaller buffers. The feature stays.

## Plan

1. **Pure core**, host-tested:
   - aggregation;
   - the segment format and its CRC;
   - cutting a bad tail;
   - rollup;
   - downsampling and `res=0`;
   - retention math;
   - Duplicate, Stale and Full.
2. **Module:**
   - files and volumes;
   - buffers;
   - the `hist` task;
   - PowerGuard;
   - the budget;
   - REST and CSV;
   - the page.
3. **UI:**
   - the `.history()` DSL;
   - `useHistory`;
   - the period picker;
   - reduce for bar, pie and table.
4. **Docs and checks:**
   - module-catalog and module-guide;
   - probes on C3, C6, S3 and ESP32.

Bench acceptance, carried over from SES-LIB-017:

- 1000 power cuts during append leave the files consistent.
- 10 000 appends leave `max_alloc` stable.

## Implementation decisions (step 2)

- **Files.**
  - The flash volume is the core's `ESPFS` (LittleFS).
  - The SD card follows in a later step: FileSystemModule will answer
    `queryInterface("fsVolumes.v1")` with its backends, and the history
    module picks the SD backend from there. No new core path is needed.
- **Segment names.** `/hist/<id>/t<tier>/<key>-<hash8>.seg`, where `key`
  comes from `segmentKey()` and `hash8` is the schema hash in hex.
  - A changed schema therefore never appends to an older file.
  - Old-schema files stay readable and are counted as `oldBytes`.
- **A torn tail at mount.** The file is rewritten without the bad tail:
  copied to `<name>.tmp` and then renamed. Arduino `File` can't truncate,
  and the name is never `.partial`. A torn tail is rare, since it needs a
  power cut mid-append.
- **Rollups after a reboot.** For each tier k > 0, tier k−1's records that
  are newer than tier k's newest are replayed into tier k's open bucket.
  The current day's rollup is therefore rebuilt from the hours already on
  file.
- **Buffers.**
  - Each tier has its own byte ring of closed records. Tier 0 holds about
    the records of 2 flush intervals plus 8; higher tiers hold 4 records.
  - Put-series hold 16 records. That is their reserve, so `Full` never
    comes from another series.
  - Buffers live in PSRAM when there is PSRAM. Without it they are in the
    internal heap with a 16 KB total cap, and a series beyond it is refused.
- **Choosing a tier for a query.** The query reads the finest tier whose
  retention still covers `from`, or the coarsest tier if none does. Rows
  queued in RAM are included. The open bucket is not.

## UI (step 3): the owner's requirements (2026-10-24)

The owner's words: "tiers in the internal flash, but I must be able to open
the history and maybe compare it".

1. **Open any series.** The History page has a viewer: pick a series, its
   columns and a period. The resolution follows the period, through the
   tiers.
2. **Compare.**
   - **(a) Two periods.** The same column over two periods, overlaid on a
     relative time axis: today vs yesterday, this week vs last week, or
     date A vs date B.
   - **(b) Several columns or series.** Shown as stacked charts that share
     the x axis, never a second y axis.
   - **(c) Width-N columns.** Pick the elements to draw (cells 5, 17, …)
     and the row's derived min, avg and max, not all N lines. The server
     computes the derived values; the query is
     `cols=mv[min],mv[avg],mv[max]`, next to `mv[5]`.
3. **`.history()` on Trend.** Charge and Statistics show the last N hours
   inline.
4. **Deep links.** `/history?series=<id>&cols=<a,b>&from=<t>&to=<t>`,
   plus `&cmp=<from2>` for comparing two periods. Page links open the
   viewer this way.

The JK gateway's planned series, per pack:

| Series | Columns | Tiers |
|---|---|---|
| `jk.cells.pN` | U16 mV ×96 (Avg), vmin (Min), vmax (Max), dv (Max) | 5 min × 3 d, 1 h × 60 d, 1 d × 2 y |
| `jk.balance.pN` | in_mah and out_mah, I16 ×10 ×96 (Sum) | 1 h × 7 d, 1 d × 1 y |
| `inv.battery` | v, i, soc, lim_chg, lim_dis, t_min, t_max, cell_min, cell_max, wh_in and wh_out (Sum, U32) | 1 min × 2 d, 15 min × 60 d, 1 h × 1 y |

That comes to about 1.3 MB per 96-cell pack, so the gateway builds with
`-D FACTORY_HISTORY_CAP_KB=1600`.
