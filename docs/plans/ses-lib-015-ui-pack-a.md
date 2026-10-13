# SES-LIB-015 — UI pack A: progress, device-TZ time axes, category bars, time of day

Status: **agreed** with the SES firmware session (2026-10-13; answers below). Spec: `esp-rack-ses-light`
`docs/design/ses-light-design.md` §8 SES-LIB-015. The owner approved this
task in the core queue. Depends on SES-LIB-002 (`TimeSync` timezone, `TzChanged`).

## 1. Motivation

Generic dashboards need four things the form DSL can't express today:

1. A progress bar whose maximum and "plan" marker come from other live values.
2. Trend x axes that show times in the **device** timezone. Today `TrendChart` prints the
   raw x number, and `DateTimeField` converts through the **browser** zone.
3. Category bars (per day of month, per month), optionally stacked, with a reference
   line.
4. A time-of-day setting (e.g. 22:30) stored as seconds since local midnight. Today
   `DateTimeField` `time` mode builds a full epoch from the browser's "today"
   (`DateTimeField.tsx:55-59`).

Consumers can't ship their own React (R7), so these belong in the framework UI.

## 2. Scope (files)

- `lib/ESPRack/include/web/FieldVocab.h`, `web/Form.h`: new type `progress`; new option
  keys (§3).
- `lib/ESPRack/src/WebManager.cpp`: `device.tz = {iana, posix}` in the manifest
  (streaming builder and `buildManifestObject`). The flash cache and ETag are re-baked on
  `TzChanged`.
- `lib/ESPRack/src/App.cpp`: subscribe to `TzChanged` → `WebManager::refreshManifestCache()`.
- `ui/src/components/dynamic-component/utils/fieldParser.ts`: the same tokens.
- `ui/src/components/dynamic-component/elements/ProgressField.tsx` (new).
- `ui/src/components/dynamic-component/elements/TrendChart.tsx`: `xFormat`, `category`,
  `stack`, `refLine`.
- `ui/src/components/dynamic-component/elements/DateTimeField.tsx`: `dtmode=tod`.
- `ui/src/components/dynamic-component/DynamicContentHandler.tsx`: routes `progress`.
- `ui/src/utils/deviceTime.ts` (new): device-TZ formatting.
- `ui/src/types/manifest.ts`: `device.tz`.
- `ui/src/components/dynamic-component/utils/fieldParser.test.ts` (new): [host] parser
  round trip.
- Demo: one "UI pack" status tab in `esp-rack-light-demo` (`LightStateService`) with every
  new builder.
- Docs: module-guide §5.3 (form DSL reference), ARCHITECTURE §4.3.

## 3. API

```cpp
// Progress: value against a max that may be live, with an optional marker.
web::Progress("today_kwh", s.todayKwh)
    .min(0).max(10)              // static range (max ignored when maxKey is set)
    .maxKey("plan_max_kwh")      // max = the sibling state key's live value
    .markerKey("plan_kwh")       // a tick at the sibling key's value ("the plan")
    .unit("kWh").format("0.0")
    .thresholds("0:error,60:warning,95:success");   // % of max -> bar colour

// Trend: x format in the device TZ, categories, stacking, reference lines.
web::Trend("nights")
    .mode("barChart")
    .xAxis("dom").category()     // x is a category key; data is replaced, not merged
    .xFormat("dom")              // epoch|time|date|datetime|dom|month
    .stack("on_min", "off_min")  // one stack: these keys on top of each other
    .refLine("plan_min", "#888", "Plan");  // horizontal line at the sibling key's value
web::Trend("power").xFormat("time");      // time series, x = epoch seconds

// Time of day: 0..86399 seconds since local midnight, no timezone conversion.
web::DateTime("on_at", s.onAtSec).timeOfDay();
```

**Wire format.** All options use the existing `;`-separated option string. New tokens are
mirrored in `FieldVocab.h` and `fieldParser.ts`:

| Token | Type | Value |
|---|---|---|
| `progress` | type | — |
| `mn`, `mx`, `f`, `unit` | progress | existing keys, reused |
| `maxKey`, `markerKey` | progress | sibling state key |
| `thr` | progress | `"<pct>:<color>,…"`, ascending |
| `xFmt` | trend | `epoch\|time\|date\|datetime\|dom\|month` |
| `cat` | trend | bare flag |
| `stack` | trend | `"key+key"`; repeatable as `stack=a+b;stack=c+d` → `"a+b\|c+d"` |
| `refl` | trend | `"key:color:label"`, several separated by `\|` |
| `dtmode=tod` | datetime | new value of the existing key |

The obvious name `ref` is taken (the action key `ActionRef`), hence `refl`. `xFmt`, `cat`,
`thr` and `refl` are short like the existing wire keys.

## 4. Behaviour

- **Device time.** The manifest carries `device.tz = {"iana": "Europe/Kyiv", "posix":
  "EET-2EEST,M3.5.0/3,M10.5.0/4"}` from `TimeSync::getTimezone()`.
  - The UI formats with `Intl.DateTimeFormat(locale, {timeZone: iana})`.
  - Without an IANA label it uses the POSIX standard offset as `Etc/GMT∓N` (no DST), and
    marks the axis "UTC±N".
  - Without any timezone it uses the browser's zone, as today.
- **Re-bake.** On `TzChanged` (loop task, only a real change, SES-LIB-002) `App` calls
  `WebManager::refreshManifestCache()`.
  - The signature changes, so the flash cache is rewritten and the ETag changes. The next
    page load gets the new zone. An open page keeps the old zone until reload.
  - The rewrite goes to temporary files that are then renamed, so a concurrent GET never
    reads half a file. Implementation step 1 checks how `writeManifestCacheToFs()` writes
    today.
- **x values.** Epoch **seconds**. A value ≥ 1e11 is taken as milliseconds. Uptime-style
  x (the template's `millis()`) keeps the raw number when no `xFmt` is set (unchanged).
- **xFmt.**
  - `time` → `HH:mm`; `date` → `dd.MM`, locale order; `datetime` → both.
  - `dom` → day of month; `month` → short month name.
  - `epoch` → automatic: `time` when the visible span is under 36 h, else `date`.
  - Tooltips show the full date and time in the device zone.
- **Category mode** (`cat`). The incoming array **replaces** the data (no timestamp merge,
  no `maxPoints` window). x is `xAxis`'s key, a string or number formatted with `xFmt` when
  numeric. Works with `barChart` and `lineChart`.
- **stack.** Bars of one group share a `stackId`. Lines ignore it.
- **refl.** A Recharts `ReferenceLine` at y = the sibling key's live value, with a label.
  It is hidden while the value is missing or non-numeric.
- **Progress.**
  - percent = (value − min) / (max − min), clamped to 0..100. `max` comes from `maxKey`
    when present and > min.
  - The marker is a thin vertical tick at `markerKey`.
  - The colour is the highest `thr` step ≤ percent; the default is `primary`.
  - Text shows "value / max unit (pct %)" with `f` formatting.
  - Read-only only (`r`); it isn't an input.
- **timeOfDay.**
  - `<input type="time" step=60>`. Value `HH:mm` ↔ seconds = h·3600 + m·60.
  - It posts an integer 0..86399. No Date object and no timezone.
  - Read-only shows `HH:mm`.
  - The backend clamps 0..86399; the updater's job, documented in module-guide.

## 5. Budgets

- UI bundle: +≤ 15 KB gz. Expected about 4–6 KB: ProgressField, deviceTime, the Trend
  options; ReferenceLine comes from recharts.
- Firmware: the manifest's `device.tz` is about 60 B per GET. The Form builders are
  header-only and cost nothing unless used. The `TzChanged` hook in App is about 0.2 KB.

## 6. Acceptance

1. [host] `fieldParser.test.ts` (jest, react-scripts): parses the option strings that the
   C++ builders emit, every new token. A trend **without** `xFmt` keeps raw x numbers:
   `millis()` stamps must not become 1970 dates. The strings are copied from a C++ emission
   snapshot, so drift fails the test. Also `deviceTime` formatting for a DST-spanning
   range in `Europe/Kyiv` with the process TZ set to `America/New_York`.
2. [build] C3/C6/S3 light-demo with the UI built (not `ESPRACK_SKIP_UI`), the demo tab
   included. The bundle size delta is reported.
3. [bench-user] A DST-spanning trend in a browser in another TZ shows device-local
   labels. A time-of-day value round-trips.

## 7. Commit plan

1. `docs(plans)`: this plan, after agreement.
2. `feat(core)`: manifest `device.tz`, re-bake on `TzChanged`.
3. `feat(web)`: `Progress`, trend options, `timeOfDay` in FieldVocab/Form.h.
4. `feat(ui)`: parser, ProgressField, TrendChart options, DateTimeField tod, deviceTime,
   and the parser test.
5. `docs`; the light-demo commit in its own repo.

## 8. Decisions (SES session, 2026-10-13)

- **Q1:** `refLine` y comes from a live sibling state key. A constant goes into the state.
- **Q2:** `xFormat("epoch")` is automatic: span < 36 h → time, else date. The explicit
  formats stay.
- **Q3:** x is epoch seconds; ≥ 1e11 means ms. **Without `xFormat` nothing changes** (raw
  numbers), and the host test checks that.
- **Q4:** a category array replaces the data on every push.
- **Q5:** without an IANA label, a fixed POSIX offset with a "UTC±N" hint. In SES the
  label is always set (the NTP TimeZones widget writes both).
- **SES use, to be covered:**
  - a month Progress with `markerKey("plan_pct")` and thresholds;
  - Trend `pw_chart` with `xFormat("time")`;
  - a daily bar chart of the month with `xFormat("dom")` or category;
  - `DateTime.timeOfDay()` for `sched_from`/`sched_to` in the device zone.
