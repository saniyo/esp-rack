#!/usr/bin/env python3
"""Release consistency check for esp-rack.

* lib/ESPRack/include/Version.h is the single source of truth for the library
  version; lib/ESPRack/library.json and the root library.json bundle must match.
* The root bundle must compile every module under modules/ (except the
  `template` skeleton) — srcFilter and -I flags.

Exit code 0 = consistent, 1 = drift (each problem printed).
Usage: python scripts/check_versions.py   (from the repo root)
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def main():
    problems = []
    vh = read("lib/ESPRack/include/Version.h")
    parts = [re.search(r"#define ESPRACK_VERSION_%s (\d+)" % k, vh) for k in ("MAJOR", "MINOR", "PATCH")]
    if not all(parts):
        print("Version.h: cannot parse ESPRACK_VERSION_*")
        return 1
    truth = ".".join(m.group(1) for m in parts)

    for rel in ("lib/ESPRack/library.json", "library.json"):
        v = json.loads(read(rel)).get("version")
        if v != truth:
            problems.append(f"{rel}: version {v!r} != Version.h {truth!r}")

    bundle = json.loads(read("library.json"))["build"]
    src = set(bundle.get("srcFilter", []))
    flags = set(bundle.get("flags", []))
    for m in sorted(os.listdir(os.path.join(ROOT, "modules"))):
        if m == "template" or not os.path.isdir(os.path.join(ROOT, "modules", m)):
            continue
        if f"+<modules/{m}/src/>" not in src:
            problems.append(f"library.json srcFilter: missing modules/{m}/src")
        if f"-Imodules/{m}/include" not in flags:
            problems.append(f"library.json flags: missing -Imodules/{m}/include")

    for p in problems:
        print("DRIFT:", p)
    if not problems:
        print(f"OK: version {truth}, bundle covers all modules")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
