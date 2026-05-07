#pragma once
#ifndef ESPRACK_VERSION_H
#define ESPRACK_VERSION_H

// Library version for esp-rack itself. Bumped manually with each release
// — semver semantics (MAJOR.MINOR.PATCH). Surfaced in /rest/uiManifest
// under device.frameworkVersion so the frontend (and AutoUpdater audits)
// can tell which framework rev is running independently of the
// consumer-app version that lives in Builder("AppName", "v1.2.3").
//
// Three orthogonal version axes the manifest exposes:
//   * device.frameworkVersion   ← THIS file (esp-rack release)
//   * device.version            ← Builder() second arg (app/firmware build)
//   * modules[].version         ← each Module's describe() — wrapper rev
//   * features[].version        ← service-level version on WebFeatureSpec
//
// A consumer can change its own deviceVersion freely without bumping
// ESPRACK_VERSION; conversely, an esp-rack library release bumps this
// even if every consumer keeps the same Builder("..., vX.Y.Z").

#define ESPRACK_VERSION_MAJOR 0
#define ESPRACK_VERSION_MINOR 1
#define ESPRACK_VERSION_PATCH 2

#define ESPRACK_VERSION_STR_HELPER(MA, MI, PA) #MA "." #MI "." #PA
#define ESPRACK_VERSION_STR_EXPAND(MA, MI, PA) ESPRACK_VERSION_STR_HELPER(MA, MI, PA)
#define ESPRACK_VERSION_STR \
    ESPRACK_VERSION_STR_EXPAND(ESPRACK_VERSION_MAJOR, ESPRACK_VERSION_MINOR, ESPRACK_VERSION_PATCH)

#endif  // ESPRACK_VERSION_H
