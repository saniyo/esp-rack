#!/usr/bin/env python3
"""Cross-module include check for esp-rack (LIB-FIX-012).

PlatformIO's Library Dependency Finder (chain mode, the default) follows every
#include it sees, without evaluating #if / __has_include. A module that includes
a header of another module therefore pulls that module into every build that
uses it, and the build breaks when that module's own dependencies aren't
declared. Rules:

* A module may include another module's header only if its library.json
  declares that module as a dependency.
* Contracts shared between modules (provider interfaces) live in the core
  (lib/ESPRack/include) and are late-bound through App, so no module needs
  another module's headers for them.

Exit code 0 = clean, 1 = violations (each printed).
Usage: python scripts/check_module_deps.py   (from the repo root)
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODULES = os.path.join(ROOT, "modules")
INCLUDE_RE = re.compile(r'^\s*#\s*(?:include|if\s+__has_include\s*\()\s*[<"]([^>"]+)[>"]', re.M)


def main():
    owner, libname, deps = {}, {}, {}
    for m in sorted(os.listdir(MODULES)):
        mp = os.path.join(MODULES, m)
        lj = os.path.join(mp, "library.json")
        if not os.path.isfile(lj):
            continue
        meta = json.load(open(lj, encoding="utf-8"))
        libname[m] = meta.get("name")
        deps[m] = {d.get("name") for d in meta.get("dependencies", [])}
        for dp, _, files in os.walk(os.path.join(mp, "include")):
            for f in files:
                if f.endswith(".h"):
                    owner.setdefault(f, m)

    problems = []
    for m in sorted(libname):
        for dp, _, files in os.walk(os.path.join(MODULES, m)):
            for f in files:
                if not f.endswith((".h", ".cpp")):
                    continue
                path = os.path.join(dp, f)
                text = open(path, encoding="utf-8", errors="replace").read()
                for inc in INCLUDE_RE.findall(text):
                    o = owner.get(os.path.basename(inc))
                    if o and o != m and libname.get(o) not in deps[m]:
                        rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
                        problems.append(f"{rel}: includes {inc} of module '{o}' ({libname.get(o)}), "
                                        f"not a dependency in modules/{m}/library.json")

    for p in problems:
        print("DEP:", p)
    if not problems:
        print(f"OK: {len(libname)} modules, every cross-module include is a declared dependency")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
