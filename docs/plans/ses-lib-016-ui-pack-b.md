# SES-LIB-016 — UI pack B: dated table columns, live message level, client time, wide integers, icons

Status: **agreed** with the SES firmware session (2026-10-13; answers below). Spec: `esp-rack-ses-light`
`docs/design/ses-light-design.md` §8 SES-LIB-016. The owner approved this task in the
core queue. Depends on SES-LIB-015 (device timezone, `deviceTime.ts`).

## 1. Motivation

- **Tables of dated records** (nights, months, events) need dates, times and durations
  shown in the device timezone. `TableField.formatCell` handles only `boolean` and
  `number`.
- **Messages** need a severity that follows live state (OK / warning / fault).
- **"Set the clock from this browser"** needs the client's time in the action request.
- **32/64-bit integer settings.** On ESP32 (esp-14.2.0, GCC 14.2; checked with `-dM -E`) `int32_t`/`uint32_t` are `long`/
  `unsigned long`, so they hit the strict generic `updateValue<T>`. That template rejects
  numeric strings, rounded doubles and out-of-range values silently (`FormBuilder.h:469`).
- **Icons.** 14 of the 18 icons SES needs aren't in the registry.

## 2. Scope (files)

- `lib/ESPRack/include/FormBuilder.h`: `updateValue` overloads for `long`,
  `unsigned long`, `long long`, `unsigned long long`.
- `lib/ESPRack/include/web/IntCoerce.h` (new, Arduino-free): the coercion rules, which
  are host-tested.
- `lib/ESPRack/include/web/FieldVocab.h`, `web/Form.h`: `Message::levelKey()`,
  `Action::withClientTime()`, and documentation of the table column type tokens.
- `ui/…/elements/TableField.tsx`: `date`, `time`, `datetime`, `duration` cells.
- `ui/…/elements/MessageField.tsx`: `levelKey`.
- `ui/…/elements/ActionField.tsx`: `clientTime`.
- `ui/…/DynamicContentHandler.tsx`: passes `getSibling` to MessageField.
- `ui/…/utils/fieldParser.ts`: tokens.
- `ui/…/utils/iconRegistry.tsx`: 14 icons.
- `ui/src/utils/deviceTime.ts`: table cell formatters.
- Tests:
  - `test-host/test/test_int_coerce/`: [host] `updateValue` coercion via `IntCoerce.h`;
  - `ui/…/utils/fieldParser.test.ts`: [host] tokens and cell formatting.
- Demo: the light-demo "UI pack" tab gets a dated table, a live-level message and a
  "Set clock from browser" action.

## 3. API

```cpp
// Table column type tokens (existing Table::col(key, label, type[, fmt])):
web::Table("nights")
    .col("start", "Start", "datetime")         // epoch s (>= 1e11 = ms), device TZ
    .col("day",   "Day",   "date")             // full date; fmt "month" -> "Oct 2026"
    .col("on_at", "On at", "time")             // HH:MM of an epoch; fmt "tod" = seconds of day
    .col("run",   "Run",   "duration", "min"); // value unit: "s" (default) | "ms" | "min"

web::Message("m_state", s.stateText).levelKey("state_level");  // level from a sibling value
web::Action("a_clock").ref("system.setTime").withClientTime("epoch", "ms");
//   -> POST …?epoch=<browser now, s>&ms=<0..999>

// FormBuilder::updateValue, new overloads (cover int32_t/uint32_t/int64_t/uint64_t on ESP):
bool updateValue(JsonObject&, const char*, long&);
bool updateValue(JsonObject&, const char*, unsigned long&);
bool updateValue(JsonObject&, const char*, long long&);
bool updateValue(JsonObject&, const char*, unsigned long long&);
```

**Wire tokens.**
- New option keys: `levelKey` (message) and `clientTime` (action, `"secParam[,msParam]"`).
- The column types `date|time|datetime|duration` are values inside the existing `cols`
  option, not new keys.

## 4. Behaviour

- **Cells.**
  - `date` → `dd.MM.yyyy` in locale order; with fmt `month` → "Oct 2026".
  - `time` → `HH:MM`; with fmt `tod` the value is seconds since midnight, with no zone.
  - `datetime` → date + `HH:MM`.
  - `duration` → "3d 4h", "2h 05m", "45m 10s", "12s" (two largest units). Negative
    values get a "−" sign.
  - A missing value or a non-number shows as before (empty / raw).
  - The zone is the device's (SES-LIB-015 `useDeviceZone`).
- **levelKey.** The severity is the sibling's live value when it's one of
  `info|warning|error|success`; otherwise the static `level`, else `info`. Sibling lookup
  works as in SES-LIB-015: live WS keys, then form fields.
- **clientTime.**
  - At fire time ActionField appends `secParam=<floor(Date.now()/1000)>` and, if given,
    `msParam=<Date.now() % 1000>` to the query, after any `withFields`.
  - It works with `.ref()` actions. With `system.setTime` this is "set the clock from this
    browser"; the arbiter still decides (operator < NTP/GPS).
- **updateValue for wide integers.** The rules live in `IntCoerce.h`:
  - A JSON integer in range is taken as is.
  - A JSON number (double) is rounded half away from zero and clamped to the type's
    range.
  - A string with an optionally signed decimal integer (spaces allowed around it) is
    parsed (strtoll/strtoull) and clamped. Any other string is ignored.
  - bool/null/object are ignored.
  - Returns true only on a change.
  - It doesn't collide with `int&` on the host (`int32_t` = `int` there). On ESP32,
    `int32_t` = `long`, so the new overload takes over from the strict template. That
    changes behaviour: numeric strings and doubles are now accepted and clamped, where
    before they were silently dropped. The `int&` overload already behaved this way, so
    this unifies the two.

## 5. Budgets

- UI +≤ 8 KB gz. Expected about 4–5 KB: 14 icons at ≈ 0.3 KB each, plus cells and
  options.
- Firmware: the overloads are inline templates (only the used ones are emitted).
  `IntCoerce.h` is header-only.

## 6. Acceptance

1. [host] `test_int_coerce`:
   - string, number, negative, double rounding, overflow clamp, non-numeric string
     ignored;
   - `long`/`unsigned long`/`long long`/`unsigned long long`;
   - the unsigned clamp at 0.
2. [host] jest:
   - `levelKey` / `clientTime` parsing;
   - cell formatting `date`/`time`/`tod`/`datetime`/`duration` in `Europe/Kyiv` with the
     process in `America/New_York`.
3. [build] C3/C6/S3 + C3 bundle, the demo with a UI build. The bundle delta is reported.
4. [bench-user]
   - a table in the device TZ from a browser in another TZ;
   - the message level following state;
   - "Set clock from browser" setting the clock through the arbiter.

## 7. Commit plan

1. `docs(plans)`: this plan, after agreement.
2. `feat(core)`: `IntCoerce.h` + `updateValue` overloads + host test.
3. `feat(web)`: `Message::levelKey`, `Action::withClientTime`, FieldVocab.
4. `feat(ui)`: cells, levelKey, clientTime, icons, jest.
5. `docs`; the light-demo commit in its own repo.

## 8. Decisions (SES session, 2026-10-13)

- **Q1:** `duration` defaults to seconds, with fmt `ms|min`, two largest units: "2h 05m",
  "45s", and "0s" for 0.
- **Q2:** `date` fmt `month` is enough; no "week".
- **Q3:** `time` with fmt `tod` is enough; no separate type.
- **Q4:** `withClientTime("epoch", "ms")`, named after `system.setTime`.
- **Q5:** a non-numeric string is ignored (no change, no error), with a `log_d` naming
  the key. The new behaviour for `int32_t` (strings and doubles clamped) is accepted.

## 9. Implementation notes

- **Icons:** 13, not 14: `Error` was already registered (imported as `ErrorIcon`). `History`
  is imported as `HistoryIcon` so it doesn't shadow the DOM type.
- **Duration** rounds to whole seconds; a negative value gets "−".
- **Measured** (C3 light-demo with the UI built):
  - UI +2.08 kB gz (CRA, against the SES-LIB-015 build; budget 8 KB);
  - C3 2,604,482 B (+5,412 with the demo's new rows and the UI);
  - C6 2,605,653; S3 2,700,834; C3 bundle 2,598,694;
  - 0 C++ warnings.
  - host: test_int_coerce passes; jest 19/19.
