# esprack-loader: `pio run -d loader -e <chip> -t upload` writes the loader
# into the table's `factory` partition and nothing else. The platform's
# upload would write it at the app offset (ota_0 in a loader table: the
# app is gone and the loader there stops at once), plus this project's
# bootloader, partition table (the 4 MB one, wrong on an 8 or 16 MB board)
# and a blank otadata. A first install: tools/flash_loader.ps1.
import csv
import sys
from os.path import isabs, join

Import("env")  # noqa: F821 (SCons)


def factory_offset():
    table = env.GetProjectOption("board_build.partitions", "")
    if not table:
        return None
    if not isabs(table):
        table = join(env.subst("$PROJECT_DIR"), table)
    with open(table, newline="") as f:
        for row in csv.reader(f):
            cells = [c.strip() for c in row]
            if len(cells) >= 4 and not cells[0].startswith("#") and cells[1:3] == ["app", "factory"]:
                return cells[3]
    return None


def refuse(target, source, env):
    sys.stderr.write(
        "Error: the loader goes into a loader table's factory partition over USB (esptool) only; "
        "a first install: esp-rack/tools/flash_loader.ps1\n"
    )
    env.Exit(1)


offset = factory_offset()
if env.subst("$UPLOAD_PROTOCOL") != "esptool" or not offset or "$ESP32_APP_OFFSET" not in env.get("UPLOADCMD", ""):
    env.AddPreAction("upload", refuse)
else:
    # The platform appended each extra image as an (offset, path) pair.
    extra = {(o, env.subst(p)) for o, p in env.get("FLASH_EXTRA_IMAGES", [])}
    flags, kept, i = list(env.get("UPLOADERFLAGS", [])), [], 0
    while i < len(flags):
        if i + 1 < len(flags) and (flags[i], flags[i + 1]) in extra:
            i += 2
            continue
        kept.append(flags[i])
        i += 1
    env.Replace(UPLOADERFLAGS=kept, UPLOADCMD=env["UPLOADCMD"].replace("$ESP32_APP_OFFSET", offset))
    if "upload" in COMMAND_LINE_TARGETS:  # noqa: F821 (SCons)
        print("esprack-loader: upload writes the loader alone, into factory at %s" % offset)
