"""compute_flavor.py — derive ESPRACK_HW_FLAVOR_STRING automatically
from each env's PIO config (board + flash size + memory_type +
board-name PSRAM hint) and inject as a -D so no per-env duplicated
manual constants need to live in platformio.ini.

Format mirrors `DeviceIdentity::hwSuffix()` so the embedded firmware
tag matches what the device sends as `flv=` in the AutoUpdate URL.

Sources of truth:
  board.build.mcu             → chip family ("esp32s3", "esp32c6")
  board.upload.flash_size     → flash MB ("16MB" → 16)
  board.build.arduino.memory_type → flash/psram bus combo:
                                  "qio_qspi" = quad flash + quad PSRAM
                                  "qio_opi"  = quad flash + OCTAL PSRAM
                                  "opi_opi"  = OCTAL flash + OCTAL PSRAM
                                  ""         = no PSRAM
  board id (e.g. "esp32-s3-devkitc1-n16r8") → PSRAM size via
                                            -n<F>r<R> regex; missing
                                            'r<N>' means no PSRAM

Mismatched binary on the wrong board panics in the bootloader (PSRAM
init), so a wrong flavor is a brick. Better to fail loud — when the
board name doesn't parse, emit a flavor with explicit "unknown" parts
so the update-server's strict matcher refuses to ship the binary
anywhere it doesn't belong.
"""

import re
import sys

Import("env")  # noqa: F821  (PlatformIO injects)

board = env.BoardConfig()  # noqa: F821

# PIO's BoardConfig.get(path) raises KeyError when the path doesn't
# exist in the manifest — boards without PSRAM (C3, classic ESP32)
# simply omit build.arduino.memory_type, so a guarded read with a
# safe default is the only way to support every board uniformly.
def _board_get(path, default=""):
    try:
        v = board.get(path)
    except KeyError:
        return default
    return default if v is None else v


# ── chip family ─────────────────────────────────────────────────────
# build.mcu = "esp32s3" / "esp32c6" / "esp32" — already lowercase, no
# dashes. Matches the Espressif convention we want in the flavor.
mcu = str(_board_get("build.mcu", "esp32")).lower()

# ── flash MB ────────────────────────────────────────────────────────
# upload.flash_size = "16MB". Some board manifests use just "16",
# strip non-digits.
flash_raw = str(_board_get("upload.flash_size", "4MB"))
flash_mb_str = re.sub(r"[^0-9]", "", flash_raw)
flash_mb = int(flash_mb_str) if flash_mb_str else 4

# ── flash bus + PSRAM presence/bus ──────────────────────────────────
# memory_type prefix = flash bus, suffix = PSRAM bus.
# "qio_opi" -> ("qio", "opi"), "qio_qspi" -> ("qio", "qspi"),
# "opi_opi" -> ("opi", "opi"), "" -> ("qio", None)  (no PSRAM)
memory_type = str(_board_get("build.arduino.memory_type", "")).lower()
if "_" in memory_type:
    flash_bus, psram_bus = memory_type.split("_", 1)
else:
    flash_bus, psram_bus = "qio", None

flash_is_octal = (flash_bus == "opi")
has_psram      = psram_bus is not None and psram_bus != ""
psram_is_octal = has_psram and psram_bus == "opi"

# ── PSRAM size from board id ────────────────────────────────────────
# Board ids in pioarduino use "-n<flashMB>r<psramMB>" suffix when
# variants differ by memory (esp32-s3-devkitc1-n16r8 → 8 MB PSRAM).
# Bare "esp32-s3-devkitc-1" without that suffix means base SKU, no
# PSRAM hint — fall back to 0.
psram_mb = 0
if has_psram:
    board_id = str(env.get("BOARD") or _board_get("name", "") or "").lower()  # noqa: F821
    m = re.search(r"-n(\d+)r(\d+)", board_id) or re.search(r"n(\d+)r(\d+)", board_id)
    if m:
        psram_mb = int(m.group(2))
    else:
        # PSRAM is enabled in build but the board id doesn't tell us
        # the size — common for vendor-private boards. Emit "x" so
        # the flavor is recognisably-malformed and the update-server
        # rejects it loudly instead of silently shipping the wrong
        # binary.
        psram_mb = 0  # treated as 'r?' below


# ── build the flavor string ─────────────────────────────────────────
# Same shape as DeviceIdentity::hwSuffix(), matches Espressif vendor
# module names (ESP32-S3-WROOM-1-N16R8V etc):
#   <chip>-n<flashMB>[r<psramMB>][v]
flavor = f"{mcu}-n{flash_mb}"
if has_psram:
    if psram_mb > 0:
        flavor += f"r{psram_mb}"
    else:
        # PSRAM present but size couldn't be inferred — annotate so
        # operator notices the gap. Update-server will refuse to
        # match an unknown-r flavor onto a real device.
        flavor += "r?"
    if psram_is_octal:
        flavor += "v"

print(f"[flavor] auto-detected: {flavor}  "
      f"(mcu={mcu} flash={flash_mb}MB"
      f"{' octal' if flash_is_octal else ''}"
      f"{' psram=' + str(psram_mb) + 'MB' if has_psram else ' no-psram'}"
      f"{' octal' if psram_is_octal else (' quad' if has_psram else '')}"
      f")")

# Inject as a CPP define — surface it to both FwTags.cpp (for the
# embedded HW_FLAVOR_TAG string) and to anything that wants a
# compile-time flavor literal. DeviceIdentity::hwSuffix() builds the
# same string at runtime; the two must match — drift = update-server
# refuses to ship the right binary.
env.Append(CPPDEFINES=[                # noqa: F821
    ("ESPRACK_HW_FLAVOR_STRING", env.StringifyMacro(flavor)),  # noqa: F821
])
