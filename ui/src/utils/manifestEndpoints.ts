import { UiManifest } from '../types';

// Collect every REST path the manifest declares — feature rest.read /
// rest.update, top-level kind='action' entries (registerAction emits
// these as feature-level restPath), per-tab restPath, per-action
// restPath nested inside feature.actions[], and the generic endpoints[]
// list (role-keyed paths, used by filesystem / OTA / etc). Walked once
// per check; manifests are small (≤30 features in practice) so a flat
// scan beats maintaining a parallel index.
function collectRegisteredPaths(manifest: UiManifest): string[] {
  const out: string[] = [];
  for (const f of manifest.features || []) {
    if (f.rest?.read)   out.push(f.rest.read);
    if (f.rest?.update) out.push(f.rest.update);
    // Top-level action entries (kind='action') have a restPath at the
    // feature level, not in a nested actions[] array. Type isn't on
    // FeatureEntry so cast for the field read; manifests always emit
    // it as a string.
    const topPath = (f as any).restPath as string | undefined;
    if (topPath) out.push(topPath);
    for (const t of f.tabs    || []) if (t.restPath) out.push(t.restPath);
    for (const a of f.actions || []) if (a.restPath) out.push(a.restPath);
    for (const e of f.endpoints || []) if (e.path)   out.push(e.path);
  }
  return out;
}

// True iff some registered path equals `url` or sits under it as a
// prefix segment. Trailing-slash and query-string tolerated. Used by
// DynamicContentHandler to swap broken FilesField / UploadField /
// ActionField widgets for an EndpointMissing fallback — the device
// composition root may have left out a backend module (commented-out
// .install<…>(), build-flavor opt-out) and we want to surface that
// inline instead of letting the widget fetch-then-fail-snackbar.
export function endpointAvailable(manifest: UiManifest, url: string): boolean {
  if (!url) return true;
  // Normalise the probe URL: strip query, ensure leading "/".
  const probeRaw = url.split('?', 1)[0];
  const probe = probeRaw.startsWith('/') ? probeRaw : '/' + probeRaw;
  const probeTrimmed = probe.endsWith('/') ? probe.slice(0, -1) : probe;

  for (const raw of collectRegisteredPaths(manifest)) {
    const reg = raw.startsWith('/') ? raw : '/' + raw;
    const regTrimmed = reg.endsWith('/') ? reg.slice(0, -1) : reg;
    if (regTrimmed === probeTrimmed) return true;
    // Probe URL `/rest/fs/upload` matches registered `/rest/fs` (segment-bound).
    if (regTrimmed.startsWith(probeTrimmed + '/')) return true;
    // Registered URL is a longer route under our probe (any leaf endpoint
    // hanging off `/rest/fs` proves the parent prefix exists).
    if (probeTrimmed.startsWith(regTrimmed + '/')) return true;
  }
  return false;
}

// Lookup a manifest action by id. Returns the owning feature id +
// the action title for diagnostic display. Used by ActionField path
// before firing the click handler — surfaces a useful error when the
// backend module the action came from isn't installed.
//
// Two registration paths to handle:
//   1. web->registerAction(spec)         — emits a TOP-LEVEL feature
//                                          with kind='action' and the
//                                          restPath at the feature
//                                          level. Most current uses
//                                          (cert.*, mqtt.attachTopic,
//                                          telegram.sendManual, …).
//   2. spec.actions[].push(WebActionSpec) — nested actions[] under a
//                                          regular feature, surfaced
//                                          by WebFeatureEntry.toJson.
//                                          Rare; for actions logically
//                                          owned by a specific feature.
export function findAction(
  manifest: UiManifest,
  actionId: string,
): { featureId: string; title?: string; restPath: string } | undefined {
  if (!actionId) return undefined;
  // Top-level kind='action' entries.
  for (const f of manifest.features || []) {
    if (f.kind === 'action' && f.id === actionId) {
      const restPath = (f as any).restPath as string | undefined;
      return { featureId: f.id, title: f.title, restPath: restPath || '' };
    }
  }
  // Nested actions[] under a feature.
  for (const f of manifest.features || []) {
    for (const a of f.actions || []) {
      if (a.id === actionId) {
        return { featureId: f.id, title: a.title, restPath: a.restPath };
      }
    }
  }
  return undefined;
}
