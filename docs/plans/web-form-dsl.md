# Web Form DSL — one fluent API for everything the web builds

> **Status:** foundation landed on `feature/web-feature-builder`.
> `web/FieldVocab.h` + `web/Form.h` written; **OTA converted** as the first
> module (parity verified). Remaining modules convert **incrementally**.
> **Goal:** every web-building call reads the same fluent way, autocompletes
> after the dot, and draws its vocabulary from ONE source of truth.

---

## The problem (audited)

The web layer had **two authoring styles**:

- **`WebFeature`** (manifest / tabs / actions) — already fluent:
  `WebFeature("ap").menu(WebMenu(...).nestUnder("wifi")).tab(WebTab(...))`.
- **`FormBuilder`** (the field schema) — static methods + **free-function
  tags** that DON'T autocomplete:
  `FormBuilder::addNumberField(fields, "cpu", AF::R, 240.0, minVal(0), unit("MHz"), icon("Bolt"))`.

Plus: `FormBuilder.h` had grown to ~1170 lines (tag structs + variadic
parsers + the class), and the audit found 9 inconsistency classes — raw JSON
bypassing the builder (SystemStatus, table rows), lambda wrappers (MQTT,
Telegram), hand-rolled table-row loops, raw `showIf`/`rowFill` strings.

A module declares its UI in **two** places — `registerManifest` (WebFeature)
and `buildForm` (FormBuilder). After this work **both are the same fluent
style**, so "everything the web builds" reads identically.

## Single source of truth — `web/FieldVocab.h`

Every field **type** token and every option-string **key** is a named
constant here and **nowhere else** in the backend. It mirrors the frontend
parser — the other half of the same contract:
`ui/src/components/dynamic-component/utils/fieldParser.ts`
(`fieldConfigurations` = per-type allowed keys; `optionParsers` = value
decoders).

- **Order-independent wire format** (verified in `fieldParser.ts`: it scans
  every `;`-segment into a key→value map; the type and `r`/`rw` are detected
  anywhere). So the fluent builder may emit segments in any order — the parsed
  result is identical. This is what makes the refactor safe.
- **TODO(codegen):** generate `fieldConfigurations.ts` FROM `FieldVocab.h` (or
  both from a shared spec) so C++ and TS can never drift. Until then the header
  is the authoritative backend copy, hand-kept in sync.

### Wire-key length review (asked for)
Keys are FIXED by the frontend parser; shortening any is a coordinated
both-sides change (do it via the codegen step, one place):

| length | keys |
|---|---|
| 1–2 | `r rw mn mx f st pl to l` |
| short | `url ref base mode cols icon unit level badge color lines` |
| medium | `accept layout legend avatar method dtmode primary tooltip confirm rowFill rowNav initial maxRows showIf options` |
| long | `colorMap maxPoints secondary loadingIf hideAvatar cardTitle uploadBase withFields refetchForm` |
| longest | `rowFillFallback` (15), `cardDeletable` (13) |

Candidates to shorten later (verbose + frequent): none are hot-path-frequent;
`cardDeletable`/`hideAvatar`/`refetchForm`/`rowFillFallback` are rare. Low ROI
to shorten now — revisit if the option-string size budget bites.

## The fluent builders — `web/Form.h`

- `FieldSpec<D>` (CRTP base) — the options EVERY type accepts: `readOnly /
  readWrite`, `label icon color showIf card cardTitle cardDeletable hideAvatar
  refetchForm loadingIf`.
- Per-type builders expose **only** their valid options (so `.min()` exists on
  `Number` but not `Text` — compile error otherwise): `Text Secret Textarea Ip
  TimeZones Checkbox Switch Button Number Slider Dropdown Radio Trend Table
  Files Upload DateTime Action Message`.
- `Form(root, name, desc)` wraps the old `createForm`; `form.add(Field…)`
  materializes a builder into the `fields` array — same value-object pattern as
  `WebFeature(...).tab(WebTab(...))`.

```cpp
web::Form form(root, "ota", "OTA Settings");
form.add(web::Switch("enabled", s.enabled).icon("SystemUpdate"));
form.add(web::Number("port", (double)s.port).min(1).max(65535).format("0").icon("Settings"));
form.add(web::Secret("pwd", s.password.c_str()).icon("Lock"));
```

Each builder is a plain value object; nothing touches the JSON until `add()`.
Produces the SAME JSON the old `add*Field` emitted (parity verified per
conversion; order-free wire format makes equivalence robust).

## Migration — incremental (chosen)

Old `FormBuilder::add*Field` stays working throughout; modules convert one at a
time, each verified (build + form JSON spot-check / on-device render).

**Module checklist** (`buildForm` conversions):
- [x] **ota** — Switch/Number/Secret (done, parity verified)
- [ ] security — JWT form (2 fields)
- [ ] ntp — settings
- [ ] ap — status + settings (IP/switch/number/text)
- [ ] wifi — status (8 TF) + scan (table + action) + settings (IP/TZ/cards)
- [ ] wireguard — status + internals
- [ ] mqtt — settings + **kill lambda wrapper** (runtime subscriptions table)
- [ ] telegram — settings/status + **kill lambda wrapper**
- [ ] system-status — resources + identity + **kill raw `root["k"]=v` JSON**
- [ ] LightStateService (demo) — settings/status/integrations; trends + tables;
      **first-class `.row()`** replaces hand-coded `createNestedObject()` loops

## Phases
- **P1** ✅ fluent layer + vocab (additive; old API untouched).
- **P2** reorg `lib/ESPRack/include/web/` — move `FormBuilder.h` (→ thin shim),
  `WebFeatureSpec.h`, `WebFeatureBuilder.h` under `web/`; one umbrella `Web.h`.
- **P3** convert modules (checklist above), verifying parity each time.
- **P4** fold in the inconsistency fixes during conversion: `Table.row()` for
  runtime rows, drop the MQTT/Telegram lambda wrappers, ban raw form JSON.
- **P5** (optional) retire `FormBuilder::add*Field` once nothing calls it.

## Notes
- Zero runtime cost: builders are compile-time/stack value objects; the emitted
  JSON (and thus RAM/heap) is unchanged. Verified on C3 (RAM flat).
- `Table.row()` (P4) is the one genuinely NEW capability — a first-class way to
  seed runtime rows, replacing the hand-coded `createNestedObject()` loops that
  the audit flagged as the main raw-JSON inconsistency.
