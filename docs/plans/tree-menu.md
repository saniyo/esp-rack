# Tree-menu — hierarchical, backend-driven side navigation

> **Status:** Phase A (backend) + Phase B (frontend) implemented on
> `feature/web-buffer-auto-sizing`. Phase C (imperative helpers) deferred.
> **Scope:** the manifest-driven side-nav becomes a recursive **tree**; the
> C++ backend is the single source of truth for module *placement*.

---

## Goal

Today the side-nav is a flat list: every `registerFeature`/`registerPage`
with a `menu.label` is one top-level row, ordered by `menu.order`. Clicking a
row opens that feature's page (which itself renders a tab strip).

We want:

1. **Hierarchy** — a module can nest *under* another to arbitrary depth
   ("drop X onto Y"). A parent row expands to reveal its children.
2. **Backend-controlled placement** — the consumer arranges the tree from C++
   (the manifest), not by editing React. "Place X under Y" == set X's
   `menu.parent = "Y"`. The frontend renders whatever the manifest declares.
3. **Tabs as leaves (opt-in)** — a multi-tab feature can additionally surface
   its own tabs as jump-to-tab leaves in the tree (`menu.expandTabs`).
4. **Pure groups** — a route-less container row ("Network") that only groups
   children and toggles expand/collapse, owning no page of its own.
5. **Zero-config backward compatibility** — with no `parent` declared anywhere,
   the tree renders the exact flat list it did before.

---

## Data model (the only new contract)

`WebMenuMeta` (`lib/ESPRack/include/WebFeatureSpec.h`) gains **two additive
fields** — both default to "off", so existing modules are untouched:

```cpp
struct WebMenuMeta {
  const char* label = nullptr;
  const char* icon  = nullptr;
  int order = 0;
  WebAuthLevel auth = WebAuthLevel::Authenticated;
  bool hidden = false;
  // ── Tree-menu hierarchy (additive) ──
  const char* parent = nullptr;   // id of parent feature/group; null = top-level
  bool expandTabs = false;        // expose this feature's own tabs as leaf children
};
```

`parent` references another entry's **feature id** (`spec.id`), not its label
or route. An **orphan parent** (id not present in the manifest — e.g. the
parent module wasn't installed) degrades gracefully: the child falls back to
top-level so nothing is ever lost from the menu.

Mirrored into the manifest TypeScript contract
(`ui/src/types/manifest.ts` → `MenuMeta`):

```ts
parent?: string;
expandTabs?: boolean;
```

---

## Phase A — backend (implemented)

The two fields are emitted by **every** entry type's `toJson`, inside the
existing `if (_spec.menu.label) { … }` block, right after `menu["hidden"]`:

```cpp
if (_spec.menu.parent)      menu["parent"]     = _spec.menu.parent;
if (_spec.menu.expandTabs)  menu["expandTabs"] = true;
```

Emitted in all four manifest contributors so any kind of entry can nest:

| File | Entry kind |
|---|---|
| `include/WebFeatureDelegate.h` | `registerFeature` (stateful settings/status) |
| `include/WebInfoEntry.h`       | `registerInfoFeature` / compound shells (e.g. `system`) |
| `include/WebPageEntry.h`       | `registerPage` (raw endpoint pages) |
| `include/WebFeatureSpec.h`     | the struct itself |

Only ~3 lines added per file; no behavioral change when the fields are unset.
The manifest is still BSS-built + flash-cached + streamed (no extra heap), and
the two short strings add a handful of bytes per nested entry only.

---

## Phase B — frontend (implemented)

Two files, both under the **single source-of-truth** `esp-rack/ui/src`
(the demo's `interface/src` is a build artefact — `scripts/sync_ui.py`
rmtree+copytree on every build; never edit it directly).

### `components/layout/LayoutMenu.tsx` — flat manifest → tree

`useMemo` builds the tree on each manifest/role change:

1. `visible = features.filter(label && !hidden)` — same gate as before.
2. `nodeOf(f)` wraps each feature into a `MenuNode { id, label, icon, to?,
   disabled, order, children[] }`.
   - `to = routePrefix(f)` strips the `/*` suffix off `route` (e.g.
     `/wifi/*` → `/wifi`). A `null` route ⇒ no `to` ⇒ pure group.
   - `expandTabs && to && tabs.length > 1` pushes one leaf per tab
     (`id = "<fid>/<tabKey>"`, `to = "<base>/<tabKey>"`). A single-tab
     feature's page *is* the tab, so expanding it is pointless — gated out.
3. `byId` map, then link each node to `byId.get(menu.parent)` (orphan parent ⇒
   root). 4. `sortRec` by `menu.order` at every level.
5. Render `roots.map(<LayoutMenuItem>)` + the standalone **Security** node
   (not in the manifest; appended explicitly, admin-gated, `order: 9999`).

### `components/layout/LayoutMenuItem.tsx` — recursive row

A single recursive `FC` renders one node and its subtree:

- **Affordance (chosen after surveying MUI/VS Code/Finder patterns):** the row
  body stays the navigation target; a separate **chevron `IconButton` on the
  right** (`secondaryAction`, `ExpandMore`/`ExpandLess`) toggles expand. This
  is the MUI-canonical split — clicking "WiFi" still opens the WiFi page, while
  the chevron reveals "Access Point" underneath. `toggle` does
  `preventDefault + stopPropagation` so it never also navigates.
- **Navigable vs group:** `to` set ⇒ `ListItemButton component={Link}`;
  route-less ⇒ `ListItemButton onClick={toggle}` (the whole row toggles).
- **Auto-open:** `subtreeActive(node, pathname)` seeds `open` and re-opens on
  route change, so deep-linking to a nested page expands the path to it.
- **Indent:** `pl: 2 + depth*2`, `pr: 6` when a chevron is present.
- **Cycle guard:** `ancestors: string[]` threads the path; children already in
  `ancestors` are filtered out — a misconfigured `parent` loop can't recurse
  forever. (Array, not `Set` — `[...ancestors, id]` downlevels cleanly; a
  `Set` spread tripped `TS2802` under the project's pre-ES2015 target.)
- Children render inside `<Collapse in={open}>`.

### Backward compatibility

No `parent` anywhere ⇒ every node is a root ⇒ identical flat list, same order,
same click-opens-page behavior. `expandTabs` defaults false ⇒ no tab leaves.
Existing consumers need zero changes.

---

## Demo (in this repo)

Two one-line manifest edits demonstrate both axes (and verify the tree on real
hardware — the C3 with no PSRAM is the cliff target):

| Edit | File | Effect |
|---|---|---|
| `spec.menu.parent = "wifi"` | `esp-rack/modules/ap/.../APSettingsService.cpp` | "Access Point" nests under "WiFi Connection" |
| `spec.menu.expandTabs = true` | `esp-rack-light-demo/src/LightStateService.cpp` | "Light" expands to its 3 tabs (Status / Integrations / Settings) as leaves |

Result: **WiFi Connection** ▸ (Access Point) and **Light** ▸ (Status,
Integrations, Settings), everything else flat as before.

---

## Phase C — deferred (imperative helpers)

The frontend already renders route-less nodes as pure groups generically, so
these are **ergonomics**, not blockers:

1. **`registerGroup(id, label, icon, order)`** — a backend helper that
   registers a `menu.label`-only entry with `route = null` so a consumer can
   declare a container ("Network") without a backing page, then point children
   at it via `parent`. (Today you'd register an empty `InfoFeature` shell.)
2. **`web->nestUnder(childId, parentId)`** — imperative placement from the
   consumer's `main.cpp` *after* modules install, so a consumer can rearrange
   **framework** modules' placement without forking the framework module's
   source (the AP-under-WiFi demo currently edits the framework module
   directly, which changes the default for all consumers). Needs an
   `IWebFeatureEntry::setMenuParent(const char*)` setter + a `WebManager`
   lookup-by-id.

Decision pending with the user on whether Phase C is wanted now or later.

---

## Verification

- **Build sanity:** `pio run -e esp32-c3-devkitm-1` (syncs `ui/src` → demo
  `interface/`, runs the React/TS build → `WWWData.h` → firmware). The TS build
  is the type-check gate for `LayoutMenu`/`LayoutMenuItem`.
- **On device:** confirm WiFi ▸ Access Point and Light ▸ (3 tabs); confirm a
  cold deep-link to `/ap` auto-expands WiFi; confirm flat entries unchanged.
- **Memory:** purely additive manifest strings; no new runtime allocation path
  — the tree is built client-side from the already-served manifest.
