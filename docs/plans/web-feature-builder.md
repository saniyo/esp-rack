# WebFeature builder — type-safe, IDE-discoverable spec authoring

> **Status:** implemented on `feature/web-feature-builder`
> (`lib/ESPRack/include/WebFeatureBuilder.h`). Demonstrated by converting
> `LightStateService` (demo) and the framework `APSettingsService`.
> **Scope:** developer ergonomics + safety. No wire-format / runtime change.

---

## Problem

`WebFeatureSpec` and friends are filled field-by-field. Every value is a bare
assignment the compiler can't check and the editor can't guide:

```cpp
WebFeatureSpec spec;
spec.id          = "lightState";
spec.component   = "DynamicSettings";   // typo -> blank page, no compile error
spec.menu.icon   = "Lightbulb";         // typo -> broken icon, no compile error
spec.menu.parent = "wifi";              // typo -> silently orphaned to top-level
spec.menu.expandTabs = true;
WebTabSpec t; t.key = "status"; t.live = true; spec.tabs.push_back(t);   // verbose
```

The **fields** already autocomplete (it's a struct). The gap is the **values**:
magic strings with no discovery and no validation — "просто параметри, які
можна криво написати".

## The three levers (escalating safety)

1. **`constexpr const char*` namespaces** — `WebComponent::DynamicSettings`,
   `WebIcon::Lightbulb`. The editor lists every option on `::`; a typo is a
   missing-symbol error; the constant *is* the canonical string (no mapping,
   negligible flash). Soft: the field stays `const char*`, so a raw string
   still compiles.
2. **`enum class`** (hard) — the field type becomes the enum, so a raw/typo
   value won't compile. Already used for `WebAuthLevel`; the model to copy for
   `component` later. Needs an enum→string map at toJson.
3. **Fluent builder** (this doc) — guided construction: id required in the
   ctor, named methods for every option, typed sub-builders, `.build()`
   validation. The strongest "no right to error".

This deliverable is **Lever 3**, using Lever-1 constants (`WebComponent::`,
`WebIcon::`) for the value sets.

## The API — `WebFeatureBuilder.h`

```cpp
auto spec = WebFeature("lightState")          // id REQUIRED (ctor)
    .title("Light Service")
    .version("0.1.0")
    .component(WebComponent::DynamicSettings)  // named, typo-proof
    .authenticated()                           // visibility = named method
    .menu(WebMenu("Light")
              .icon(WebIcon::Lightbulb)
              .order(50)
              .expandTabs())                   // tabs -> tree leaves
    .rest(LIGHT_SETTINGS_PATH)                 // GET+POST share the path
    .ws(LIGHT_SETTINGS_SOCKET_PATH)
    .tab(WebTab("status", "Status").rest(LIGHT_SETTINGS_PATH).live())
    .tab(WebTab("settings", "Settings").rest(LIGHT_SETTINGS_PATH).postable())
    .build();                                  // -> WebFeatureSpec (moved out)
web->registerFeature<LightState>(std::move(spec), this, ...);
```

Builders & their named options:

| Builder | ctor | key methods |
|---|---|---|
| `WebFeature(id)` | id | `.title .version .component .menu .auth/.adminOnly/.authenticated/.publicAccess .rest/.readOnly .ws .route .tab .action .build` |
| `WebMenu(label)` | label | `.icon .order .nestUnder(parentId) .expandTabs .hidden .auth/.adminOnly/…` |
| `WebTab(key,title)` | key,title | `.rest .postable .live .order .auth/.adminOnly/…` |
| `WebAction(id,title)` | id,title | `.icon .color .rest .method .confirm .success .handler .internalHandler .auth/…` |

Sub-builders (`WebMenu`/`WebTab`/`WebAction`) implicitly convert to their spec,
so they drop into `.menu()`/`.tab()`/`.action()` without an explicit `.build()`.
Tree placement reads as intent: **`.nestUnder("wifi")`** == "drop this onto the
WiFi feature".

## Properties

- **Additive / backward-compatible.** The structs are untouched; struct-style
  registration keeps working. The builder is just a nicer front door yielding
  the *same* `WebFeatureSpec`.
- **Zero runtime cost.** Verified on C3: RAM unchanged (the builder is
  compile-time/stack-only; the produced spec is byte-identical). The wire
  format is unchanged — `toJson` emits the same manifest JSON, so the UI is
  identical. Flash +~0.5 KB from the `WebIcon`/`WebComponent` string set.
- **Discoverable.** `WebFeature("x").` lists every option; `WebIcon::` /
  `WebComponent::` list every known value.

## Before / after (LightState)

| | before | after |
|---|---|---|
| construct | `WebFeatureSpec spec; spec.x = …;` ×13 + 3× `WebTabSpec t; …; push_back` | one `WebFeature(...).…` chain |
| icon/component | raw strings | `WebIcon::` / `WebComponent::` |
| placement | `spec.menu.parent = "wifi"` | `.nestUnder("wifi")` |
| typo safety | none (silent runtime breakage) | named methods + constants |

## Future (not in this pass)

- **Generate `WebIcon` from the frontend icon registry** so the C++ shortcuts
  and the TS registry can't drift (fold into the `sync_ui` pipeline). Until
  then `WebIcon` is a curated subset; any MUI name still works as a raw string.
- **`WebComponent` as a real `enum class`** (Lever 2 hard enforcement) once the
  component set is frozen — mirrors `WebAuthLevel`.
- **Per-module `FeatureId` constants** (`WiFiFeature::Id`) so `.nestUnder()`
  takes a symbol, not a string — makes the cross-module dependency explicit and
  greppable, and a typo'd parent a compile error.
- **`WebPage` / `WebEndpoint` builders** for `registerPage` (raw-endpoint pages)
  — same pattern, deferred until a consumer needs it.
