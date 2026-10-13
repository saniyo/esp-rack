"""sync_ui.py — populate ./interface/ from canonical ../esp-rack/ui/.

Convenience: the consumer drops a single PWA icon at
    interface_overrides/public/app/icon.png
and sync auto-generates the matching `favicon.ico` (PNG wrapped in an
ICO container — every modern browser accepts that). A separate
favicon.ico file is honoured if explicitly placed. If Pillow is
available the icon is also auto-downscaled to TARGET_ICON_SIZE so a
2048×2048 download doesn't bloat the firmware bundle by ~1 MB; the
original in overrides/ is left untouched, only the copy that lands
in interface/ gets resized.

Architectural contract: the React frontend lives ONLY in the library
(esp-rack/ui/). The demo has zero per-service React — every C++ service
declares its UI via FormBuilder/WebFeatureSpec, and the library renders
dynamically. So the consumer's `interface/` is a build artefact, not a
source of truth.

This script is the bridge: pre:build it copies the library tree into
`./interface/` so build_interface.py can run npm against a complete
project. Re-running is idempotent.

The consumer can override individual files by dropping them under
./interface_overrides/ — that overlay is applied AFTER the library
copy. The script ALSO mirrors the lib's `public/` directory shape into
./interface_overrides/ on first run, so the consumer sees pre-made
empty target directories and knows where to drop project-branding
assets (icon.png, favicon.ico, theme overrides) without first reading
this source. Existing files inside overrides are NEVER touched — only
missing dirs are created.

Failure modes:
  * `../esp-rack/ui/` missing -> fail fast with a clear message.
  * `interface/node_modules` and `interface/build` are PRESERVED
    across syncs (npm install + webpack output, expensive to redo).
"""

from pathlib import Path
from shutil import copy2, copytree, rmtree
import os
import struct
import sys

Import("env")  # noqa: F821  (PlatformIO injects)

ROOT          = Path.cwd()
INTERFACE_DIR = ROOT / "interface"
OVERRIDES_DIR = ROOT / "interface_overrides"

# Resolve where the canonical UI lives. Two supported modes:
#   1. Local-dev — sibling clone: ../esp-rack/ui/
#      (lib_extra_dirs in platformio.ini points at ../esp-rack/lib +
#      ../esp-rack/modules; we mirror that convention for UI)
#   2. Git-pull — released library cloned by PIO into
#      .pio/libdeps/<env>/ESPRack/ui/ via lib_deps = github URL
# Local sibling wins when present so active framework dev iterates
# without re-tagging releases. PIO-cloned location is the fallback,
# resolved per build env via env["PIOENV"].
def _lib_extra_dirs() -> list:
    """The project's lib_extra_dirs entries, as written in platformio.ini."""
    raw = env.GetProjectOption("lib_extra_dirs", "")  # noqa: F821
    items = [str(p) for p in raw] if isinstance(raw, (list, tuple)) else str(raw or "").splitlines()
    return [p.strip() for p in items if p.strip()]


def _local_library_root():
    """Local-dev mode: the lib_extra_dirs entry that holds ESPRack/library.json
    is <esp-rack>/lib, so its parent is the library root. Any folder name and
    any location (sibling or not, relative or absolute) — SES-LIB-003."""
    for entry in _lib_extra_dirs():
        p = Path(os.path.expanduser(entry))
        if not p.is_absolute():
            p = ROOT / p
        if (p / "ESPRack" / "library.json").is_file():
            return p.resolve().parent
    return None


def _is_local_dev_mode() -> bool:
    """Local-dev (lib_extra_dirs points at an esp-rack checkout) vs released
    mode (lib_deps git URL; PIO clones the library into .pio/libdeps)."""
    return _local_library_root() is not None


def _resolve_library_root() -> Path:
    """Return the root of esp-rack (the dir containing lib/, modules/, ui/):
    local-dev -> the lib_extra_dirs checkout; released -> the PIO clone at
    .pio/libdeps/<env>/ESPRack/. A leftover clone from a previous released-
    mode build is ignored (and purged) while lib_extra_dirs points elsewhere.
    """
    local = _local_library_root()
    if local is not None:
        return local
    return ROOT / ".pio" / "libdeps" / env["PIOENV"] / "ESPRack"  # noqa: F821

def _purge_stale_cloned_lib() -> None:
    """When the demo is in local-dev mode, a leftover `.pio/libdeps/<env>/
    ESPRack/` from a previous released-mode build creates two libraries
    named "ESPRack" in PIO's LDF view (sibling + cloned), each with its
    own library.json. PIO's compat=strict resolution then becomes
    non-deterministic: usually picks one, sometimes warns, occasionally
    links against the wrong one. Pre-emptively clean the cloned copy
    so local-dev builds always resolve against the sibling.
    """
    if not _is_local_dev_mode():
        return
    pio_env = env["PIOENV"]  # noqa: F821
    stale = ROOT / ".pio" / "libdeps" / pio_env / "ESPRack"
    if stale.exists():
        rmtree(stale)
        print(f"[sync_ui] removed stale cloned lib at {stale}")


_purge_stale_cloned_lib()
LIBRARY_ROOT = _resolve_library_root()
LIBRARY_UI   = LIBRARY_ROOT / "ui"

# The UI bundle (WWWData.h) belongs to THIS consumer (SES-LIB-003): webpack's
# progmem-generator writes it to <project>/.pio/www/ (ESPRACK_WWW_DIR, read
# by ui/config-overrides.js) and that dir is prepended to the include path, so
# App.cpp's `#include <WWWData.h>` finds it — libraries are built with the
# project env's CPPPATH. Two consumers sharing one esp-rack checkout no
# longer overwrite each other's bundle and branding. One dir per consumer,
# not per env: the bundle doesn't depend on the env (flavor only changes C++
# defines), ESPRACK_SKIP_UI builds of the other envs reuse it, and it
# survives PIO wiping .pio/build when platformio.ini changes.
WWW_DIR = (ROOT / ".pio" / "www").resolve()
WWW_DIR.mkdir(parents=True, exist_ok=True)
os.environ["ESPRACK_WWW_DIR"] = str(WWW_DIR)
os.environ.pop("ESPRACK_INCLUDE_DIR", None)  # the old scheme's variable
env.Prepend(CPPPATH=[str(WWW_DIR)])  # noqa: F821

# Files at the top level of ../esp-rack/ui/ — flat copy. Anything not
# listed here is ignored (e.g. node_modules / build live elsewhere).
LIBRARY_PAYLOAD_FILES = [
    "config-overrides.js",
    "package.json",
    "progmem-generator.js",
    "tsconfig.json",
]

# Top-level subdirs that are FULLY refreshed on each sync. node_modules/
# and build/ are NOT in this list — they live inside `interface/` but
# are populated by npm and survive the sync untouched.
LIBRARY_PAYLOAD_DIRS = [
    "src",
    "public",
]


def fail(msg: str) -> None:
    print(f"[sync_ui] ERROR: {msg}", file=sys.stderr)
    raise SystemExit(1)


def _progmem_www() -> bool:
    flags = env.ParseFlags(env["BUILD_FLAGS"])  # noqa: F821
    for define in flags.get("CPPDEFINES", []):
        if define == "PROGMEM_WWW" or (isinstance(define, (list, tuple)) and define[0] == "PROGMEM_WWW"):
            return True
    return False


def check_bundle_location() -> None:
    """The old scheme wrote WWWData.h into the shared library; a copy left
    there would hide which consumer's bundle a build embeds. Fail loudly."""
    stale = LIBRARY_ROOT / "lib" / "ESPRack" / "include" / "WWWData.h"
    if stale.exists():
        fail("stale UI bundle from the old build scheme (SES-LIB-003): " + str(stale) + "\n"
             "  Bundles now live in each consumer's .pio/www/. Once every consumer of this\n"
             "  esp-rack checkout uses the new scripts, delete it (PowerShell or bash):\n"
             "    rm \"" + str(stale) + "\"")
    if os.environ.get("ESPRACK_SKIP_UI") and _progmem_www() and not (WWW_DIR / "WWWData.h").exists():
        fail(f"ESPRACK_SKIP_UI is set but {WWW_DIR / 'WWWData.h'} doesn't exist: "
             f"build once without ESPRACK_SKIP_UI.")


def replace_dir(src: Path, dst: Path) -> None:
    if dst.exists():
        rmtree(dst)
    copytree(src, dst)


# Files that get image-aware copy (resize-if-too-large) instead of
# verbatim copy2. Relative to the overlay root (interface_overrides/).
RESIZE_ON_OVERLAY = {
    "public/app/icon.png",
}


def overlay_dir(src: Path, dst: Path) -> None:
    if not src.exists():
        return
    for cur, _dirs, files in os.walk(src):
        rel = Path(cur).relative_to(src)
        target = dst / rel
        target.mkdir(parents=True, exist_ok=True)
        for f in files:
            src_file = Path(cur) / f
            dst_file = target / f
            rel_key = (rel / f).as_posix()
            if rel_key in RESIZE_ON_OVERLAY:
                maybe_resize_png(src_file, dst_file)
            else:
                copy2(src_file, dst_file)


# Per-file branding seeds — copied from lib into interface_overrides/
# when the corresponding override is missing (first sync OR user
# accidentally deleted the file). Existing overrides are NEVER
# overwritten — the consumer's customised PNG / ICO stays put across
# syncs. Restricted to branding-class assets: re-shipping index.html
# or src/*.tsx would silently fork the lib UI and the consumer would
# stop receiving framework UI updates without knowing it.
#
# favicon.ico intentionally NOT in this list — it's auto-derived from
# icon.png via wrap_png_as_ico() so the consumer only needs to drop
# one file. Place a separate favicon.ico in overrides explicitly to
# override the auto-derived one.
OVERRIDES_FILE_SEEDS = [
    "public/app/icon.png",
    "public/app/manifest.json",
]


# Target icon resolution — matches the largest entry declared in
# ui/public/app/manifest.json (`sizes:"...256x256"`). Browser picks
# down from this for smaller render contexts (Add to Home Screen,
# tab favicon). Bumping past 256×256 inflates firmware by 4× per
# step (PNG is roughly area-quadratic for solid art) for no visual
# benefit on the typical screen DPI a smart-home dashboard sees.
TARGET_ICON_SIZE = (256, 256)
_pillow_warned = False


def maybe_resize_png(src_path: Path, dst_path: Path) -> bool:
    """Copy `src_path` → `dst_path`. If Pillow is available AND the
    source is larger than TARGET_ICON_SIZE, downscale (preserving
    aspect ratio) before writing. Returns True iff a resize actually
    happened. On Pillow-absent systems the file is copied verbatim
    and a one-time install-hint is printed.
    """
    global _pillow_warned
    try:
        from PIL import Image  # type: ignore
    except ImportError:
        # Bigger than 50 KB is the practical breakpoint where the
        # firmware bundle starts to feel it. Below that the copy-as-is
        # path is fine and we don't pester the user.
        if (src_path.stat().st_size > 50 * 1024) and not _pillow_warned:
            print(f"[sync_ui] WARN: {src_path.name} is "
                  f"{src_path.stat().st_size // 1024} KB — "
                  f"`pip install Pillow` to enable auto-downscale")
            _pillow_warned = True
        copy2(src_path, dst_path)
        return False

    img = Image.open(src_path)
    if img.size[0] <= TARGET_ICON_SIZE[0] and img.size[1] <= TARGET_ICON_SIZE[1]:
        copy2(src_path, dst_path)
        return False

    orig = img.size
    # thumbnail() shrinks in-place preserving aspect; final image
    # fits inside TARGET_ICON_SIZE. LANCZOS is the standard high-
    # quality downsampling filter.
    img.thumbnail(TARGET_ICON_SIZE, Image.LANCZOS)
    # Force RGBA so transparent corners / alpha logos survive the
    # save-roundtrip even if the source was indexed-PNG.
    if img.mode != 'RGBA':
        img = img.convert('RGBA')
    img.save(dst_path, format='PNG', optimize=True)
    print(f"[sync_ui] resized {src_path.name}: "
          f"{orig[0]}×{orig[1]} -> {img.size[0]}×{img.size[1]}")
    return True


def wrap_png_as_ico(png_bytes: bytes) -> bytes:
    """Wrap a single PNG payload in a 1-entry ICO container. Every
    modern browser (Chrome/Edge/Firefox/Safari) reads the embedded PNG
    directly — no rasterisation needed. The width/height byte fields
    are reported as 64×64 because that's what 'most' icons end up at;
    actual rendering uses the PNG's own dimensions.
    """
    ico_header = struct.pack('<HHH', 0, 1, 1)        # reserved=0, type=ICO, count=1
    ico_dir = struct.pack('<BBBBHHII',
        64,                # width  (0 means 256)
        64,                # height
        0,                 # color count
        0,                 # reserved
        1,                 # color planes
        32,                # bits per pixel
        len(png_bytes),    # size of image data
        22)                # offset (header + 1 directory entry)
    return ico_header + ico_dir + png_bytes


def ensure_overrides_skeleton() -> None:
    """Make ./interface_overrides/ self-bootstrap from the library:
      1. Mirror lib `ui/public/` directory shape — missing dirs are
         created, existing dirs left alone. Lib-driven: when the lib
         adds public/fonts/, the consumer's overrides picks it up on
         the next sync without editing this script.
      2. Seed missing branding assets (OVERRIDES_FILE_SEEDS) by
         copying the lib default into overrides. Idempotent per-file:
         a customised override stays put; an accidentally deleted
         override gets restored from lib on the next sync — so a
         broken project always heals to a working default rather
         than 404-ing on the missing favicon.
    Never overwrites existing files. Never deletes anything from
    overrides.
    """
    src_root = LIBRARY_UI / "public"
    if not src_root.is_dir():
        return
    OVERRIDES_DIR.mkdir(parents=True, exist_ok=True)

    created_dirs = 0
    for cur, _dirs, _files in os.walk(src_root):
        rel = Path(cur).relative_to(LIBRARY_UI)  # "public", "public/app", ...
        target = OVERRIDES_DIR / rel
        if not target.exists():
            target.mkdir(parents=True, exist_ok=True)
            created_dirs += 1

    seeded_files = 0
    for rel_path in OVERRIDES_FILE_SEEDS:
        src = LIBRARY_UI / rel_path
        dst = OVERRIDES_DIR / rel_path
        if src.is_file() and not dst.exists():
            dst.parent.mkdir(parents=True, exist_ok=True)
            copy2(src, dst)
            seeded_files += 1

    # Auto-derive favicon.ico from icon.png unless the consumer placed
    # an explicit favicon.ico already. Re-run when icon.png changes is
    # NOT triggered here (that would clobber an explicit favicon —
    # implicit derivation only fills a hole). Operator who customises
    # icon.png and wants the favicon refreshed should `rm favicon.ico`
    # and re-sync.
    derived_favicon = False
    icon_src = OVERRIDES_DIR / "public" / "app" / "icon.png"
    favicon_dst = OVERRIDES_DIR / "public" / "favicon.ico"
    if icon_src.is_file() and not favicon_dst.exists():
        favicon_dst.parent.mkdir(parents=True, exist_ok=True)
        favicon_dst.write_bytes(wrap_png_as_ico(icon_src.read_bytes()))
        derived_favicon = True

    if created_dirs or seeded_files or derived_favicon:
        derived_str = ", favicon auto-derived" if derived_favicon else ""
        print(f"[sync_ui] seeded interface_overrides/ "
              f"({created_dirs} dir(s), {seeded_files} file(s)"
              f"{derived_str}) — edit in place to customise")


def sync() -> None:
    # Parallel-flash escape hatch: when the UI has already been staged by a
    # prior build in the same flash run, downstream per-env firmware builds /
    # uploads set ESPRACK_SKIP_UI=1 so they DON'T rmtree+recopy interface/
    # again. That both removes the redundant work and prevents concurrent
    # builds from racing on the shared interface/ tree. Unset by default, so
    # a normal `pio run` is unaffected.
    if os.environ.get("ESPRACK_SKIP_UI"):
        print("[sync_ui] skipped (ESPRACK_SKIP_UI set; interface/ already staged)")
        return
    if not LIBRARY_UI.is_dir():
        fail(f"canonical UI not found at {LIBRARY_UI}; point lib_extra_dirs at <esp-rack>/lib "
             f"and <esp-rack>/modules, or use lib_deps with the esp-rack git URL")

    INTERFACE_DIR.mkdir(parents=True, exist_ok=True)

    # Make sure interface_overrides/ skeleton exists BEFORE the overlay
    # step. First-run consumer thus gets empty target dirs created
    # automatically (lib-driven shape); subsequent runs are no-ops if
    # the skeleton is intact.
    ensure_overrides_skeleton()

    for f in LIBRARY_PAYLOAD_FILES:
        src = LIBRARY_UI / f
        if not src.exists():
            print(f"[sync_ui] note: {f} missing from library; skipping")
            continue
        copy2(src, INTERFACE_DIR / f)

    for d in LIBRARY_PAYLOAD_DIRS:
        src = LIBRARY_UI / d
        if not src.exists():
            print(f"[sync_ui] note: {d}/ missing from library; skipping")
            continue
        replace_dir(src, INTERFACE_DIR / d)

    overlay_dir(OVERRIDES_DIR, INTERFACE_DIR)

    print(f"[sync_ui] synced {LIBRARY_UI} -> {INTERFACE_DIR}")


check_bundle_location()
sync()
