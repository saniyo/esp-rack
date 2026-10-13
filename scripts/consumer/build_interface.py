from pathlib import Path
from shutil import copytree, rmtree, copyfileobj
from subprocess import check_output, Popen, PIPE, STDOUT, CalledProcessError
import os
import gzip

Import("env")

def gzipFile(file):
    with open(file, 'rb') as f_in:
        with gzip.open(file + '.gz', 'wb') as f_out:
            copyfileobj(f_in, f_out)
    os.remove(file)

def flagExists(flag):
    buildFlags = env.ParseFlags(env["BUILD_FLAGS"])
    for define in buildFlags.get("CPPDEFINES"):
        if (define == flag or (isinstance(define, list) and define[0] == flag)):
            return True

def buildWeb():
    os.chdir("interface")
    print("Building interface with npm")

    # No REACT_APP_PROJECT_NAME injection — the brand shown in the UI
    # comes from /rest/uiManifest.device.name (set by Builder("...","..."))
    # at runtime. Single source of truth in main.cpp; the React bundle
    # stays consumer-agnostic.

    # Source maps are pure dev-time aid; in PROGMEM_WWW they get baked
    # into firmware (~6 MB) for zero runtime benefit. CRA respects this
    # env var at build time.
    os.environ["GENERATE_SOURCEMAP"] = "false"
        
    try:
        env.Execute("npm install")
        env.Execute("npm run build")
        buildPath = Path("build")
        wwwPath = Path("../data/www")
        if wwwPath.exists() and wwwPath.is_dir():
            rmtree(wwwPath)        
        if not flagExists("PROGMEM_WWW"):
            print("Copying interface to data directory")
            copytree(buildPath, wwwPath)
            for currentpath, folders, files in os.walk(wwwPath):
                for file in files:
                    gzipFile(os.path.join(currentpath, file))
    finally:
        os.chdir("..")

if os.environ.get("ESPRACK_SKIP_UI"):
    # Parallel-flash escape hatch (see sync_ui.py): the UI bundle / WWWData.h
    # was already produced by a prior build in this flash run, so per-env
    # firmware builds and uploads skip the expensive npm rebuild and reuse it.
    print("[build_interface] skipped npm build (ESPRACK_SKIP_UI set)")
elif (len(BUILD_TARGETS) == 0 or "upload" in BUILD_TARGETS):
    buildWeb()
else:
    print("Skipping build interface step for target(s): " + ", ".join(BUILD_TARGETS))
