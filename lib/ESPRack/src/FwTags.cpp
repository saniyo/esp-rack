// FwTags.cpp — embeds three "well-known" tag strings into the
// firmware binary so the update-server's upload route can identify
// {project, hardware flavor, version} via a regex grep over the
// raw .bin. The server uses these to refuse a binary onto a board
// whose memory layout (octal PSRAM / flash variant / size) doesn't
// match what the firmware was compiled for — a mismatch would
// panic in the bootloader before app code runs.
//
// Server-side parser (esp-update-server-master/app/routes/upload.py):
//   BASE_PLATFORM_TAG_([A-Za-z0-9_-]{1,64})
//   HW_FLAVOR_TAG_([A-Za-z0-9_-]{1,64})
//   VERSION_TAG_(v\d+\.\d+\.\d+)
//
// Tag values come from consumer's factory_settings.ini + per-env
// platformio.ini:
//   FACTORY_PROJECT_NAME      → BASE_PLATFORM_TAG (lowercased here)
//   FACTORY_PROJECT_VERSION   → VERSION_TAG
//   ESPRACK_HW_FLAVOR_STRING  → HW_FLAVOR_TAG  (per-env, e.g.
//                                "esp32s3-n16r8v")
//
// All three are required for the upload route to file the binary
// under platforms[base].firmwares[flv] = {version, file, ...}.
// Missing any one → upload falls back to legacy single-file mode
// and the auto-update strict-flavor path on the device side cannot
// pick the right artefact.

// Hook into the macros' values without expanding them in this file.
// We need the literal C-string token, so XSTR() forces one
// preprocessor expansion (turning FACTORY_PROJECT_NAME into
// "ESPRackDemo") before string-pasting.
#define _ESPRACK_STR(x) #x
#define _ESPRACK_XSTR(x) _ESPRACK_STR(x)

#ifndef FACTORY_PROJECT_NAME
#define FACTORY_PROJECT_NAME "ESPRack"
#endif
#ifndef FACTORY_PROJECT_VERSION
#define FACTORY_PROJECT_VERSION "v0.0.0"
#endif
#ifndef ESPRACK_HW_FLAVOR_STRING
// Last-resort fallback so the build doesn't fail when a consumer's
// platformio.ini env forgot to set this — but the upload route will
// reject "unset" as an unknown flavor, which is the right failure
// mode (loud + visible) versus pretending an unspecified flavor is
// compatible with whatever board picks up the firmware.
#define ESPRACK_HW_FLAVOR_STRING "unset"
#endif

// Two steps to keep the strings in the final image:
//   1. `extern` keyword on each definition + initialiser is the C++
//      idiom for "external-linked const definition". Without
//      `extern`, a `const T x = …` global has INTERNAL linkage by
//      default in C++ (even inside an `extern "C"` block — that
//      block only sets the mangling, not the linkage). Internal-
//      linkage symbols can be dropped under --gc-sections.
//   2. `__attribute__((used))` keeps the compiler from eliding the
//      storage as unreferenced before the linker even sees it.
// App.cpp performs an explicit Serial.printf read of each symbol,
// which gives the linker an undefined reference that pulls this
// TU's .o out of the archive.
extern "C" {

__attribute__((used))
extern const char ESPRACK_TAG_BASE[] = "BASE_PLATFORM_TAG_" FACTORY_PROJECT_NAME;

__attribute__((used))
extern const char ESPRACK_TAG_FLV[]  = "HW_FLAVOR_TAG_"     ESPRACK_HW_FLAVOR_STRING;

__attribute__((used))
extern const char ESPRACK_TAG_VER[]  = "VERSION_TAG_"       FACTORY_PROJECT_VERSION;

}  // extern "C"
