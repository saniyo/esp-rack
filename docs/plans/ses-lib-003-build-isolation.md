# SES-LIB-003 — Upstream build isolation

Status: **agreed** with the SES firmware session (2026-10-13; answers below). Spec: `esp-rack-ses-light`
`docs/design/ses-light-design.md` §8 SES-LIB-003. The owner approved this task in the core
queue.

## 1. Problem

- The consumer's `sync_ui.py` points webpack's progmem generator at the esp-rack library's
  `lib/ESPRack/include/WWWData.h`. Two consumers sharing one esp-rack checkout (light-demo
  and SES) therefore overwrite each other's UI bundle and branding.
- A build reads whichever bundle was written last.
- The library root is found by the hard-coded sibling name `../esp-rack`, so a non-sibling
  checkout doesn't work.

## 2. Behaviour

- **Library root:** the first `lib_extra_dirs` entry `E` (relative to the project) for
  which `E/ESPRack/library.json` exists gives `root = E/..`, and `root/ui` must exist.
  Without such an entry (released mode, `lib_deps` git URL), the root is
  `.pio/libdeps/<env>/ESPRack`. The folder can be named anything.
- **The bundle goes to the consumer:** `<project>/.pio/www/WWWData.h` (Q1).
  - `sync_ui.py` (a `pre:` script) runs `env.Prepend(CPPPATH=[<project>/.pio/www])`, so
    `App.cpp`'s `#include <WWWData.h>` resolves there. Library builders inherit the
    project env's CPPPATH; the build checks that.
  - It exports `ESPRACK_WWW_DIR` for `npm run build`.
- **Guard:** a `WWWData.h` under `<root>/lib/ESPRack/include` fails the build. The message
  says it comes from the old scheme and to delete it.
- **`ESPRACK_SKIP_UI=1`** without `<project>/.pio/www/WWWData.h` fails with "build once
  without ESPRACK_SKIP_UI", instead of a compiler error.
- **`ui/config-overrides.js`:**
  - `ESPRACK_WWW_DIR` → `<dir>/WWWData.h`.
  - Legacy `ESPRACK_INCLUDE_DIR` (old consumer scripts) still works, with a warning, so a
    pinned consumer keeps building during migration.
  - Neither set (a manual `npm run build` inside `interface/`) → `../.pio/www/WWWData.h`.
- **Canonical scripts:** `esp-rack/scripts/consumer/{sync_ui.py, build_interface.py,
  compute_flavor.py}`. The demo's `scripts/` become copies of them. A consumer copies them
  too (consumer-guide, step 18b).
- **`lib/ESPRack/include/.gitignore`:** `WWWData*.h`.
- **consumer-guide:** a migration note for pinned consumers.
  1. Copy the three scripts.
  2. Delete the stale `esp-rack/lib/ESPRack/include/WWWData.h` once every consumer of
     that checkout has migrated.
  3. `.pio/` is already ignored.

## 3. Acceptance

1. [build] Demo → a throwaway copy of the demo with a different project name and icon →
   demo again. Each `.pio/www/WWWData.h` keeps its own icon, and the demo's isn't touched
   by the copy's build.
2. [build] The copy builds from a non-sibling path (absolute `lib_extra_dirs`).
3. [build] Released/bundle mode (`lib_deps = file://…`, like a git URL).
4. [build] `ESPRACK_SKIP_UI=1` for the second env reuses the bundle.

## 4. Decisions (SES session, 2026-10-13)

- **Q1:** `<project>/.pio/www`, one per consumer. The bundle doesn't depend on the flavor
  (`compute_flavor.py` only adds C++ defines; `flash-all.ps1` already relies on one
  bundle for every target), so there's no `<flavor>/` subdirectory.
- **Q2:** the guard fails, and the message gives the exact path and one `rm` command
  (PowerShell or bash). The scripts never delete anything in the shared checkout.
- **Q3:** SES migrates its own scripts from `scripts/consumer/`; the report carries a
  checklist (consumer-guide §5.1). `check_esprack.py` stays in SES.
