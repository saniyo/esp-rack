import { FC, useCallback, useEffect, useMemo, useState } from 'react';

import * as ManifestApi from '../../api/manifest';
import { configureFsEndpoints } from '../../api/fs';
import { RequiredChildrenProps, extractErrorMessage } from '../../utils';
import { FeatureEntry, UiManifest } from '../../types';

import { ManifestContext, ManifestContextValue } from './context';
import ManifestProgress from './ManifestProgress';

const EMPTY_MANIFEST: UiManifest = {
  schemaVersion: 0,
  features: []
};

// Map feature entries to the module-level path tables that non-hook code
// (api/fs.ts, …) reads from. Called whenever the manifest changes.
const applyManifest = (manifest: UiManifest): void => {
  const fs = manifest.features.find((f) => f.id === 'filesystem');
  configureFsEndpoints(fs?.endpoints);
};

// The manifest must be loaded BEFORE children mount. An earlier design
// kept children mounted under the overlay so they could "warm up" in
// the background, but that created a race: any DynamicFeature that
// rendered with an empty manifest captured `entry=undefined`, kicked
// its own fetch chain, and on the eventual context update the partial
// state could leave inner FormLoaders stuck waiting on a tab REST that
// fired against the wrong endpoint. Gating children on `loaded` (or
// the error fallback after the overlay's hold) eliminates that window
// entirely — by the time any DynamicFeature mounts, the manifest is
// final and `findFeature(id)` always returns the real entry on first
// render.
const ManifestLoader: FC<RequiredChildrenProps> = (props) => {
  const [manifest, setManifest] = useState<UiManifest>(EMPTY_MANIFEST);
  const [loaded, setLoaded] = useState<boolean>(false);
  const [error, setError] = useState<string | undefined>();
  const [revealComplete, setRevealComplete] = useState<boolean>(false);

  const loadManifest = useCallback(async () => {
    // Reset both flags at start. `loaded=false` parks Authenticated
    // routing on the spinner; `revealComplete=false` un-hides the
    // ManifestProgress carousel so the operator sees the per-module
    // probe walk-through on EVERY reload, not just the first mount.
    // Without this, signIn() → reloadManifest() did its work in the
    // background but the sidebar stayed stale (old empty stub) until
    // a route change forced a re-render and even then the user saw
    // no transition cue.
    setLoaded(false);
    setRevealComplete(false);

    // Retry loop. The single biggest source of "title=ESPRack +
    // empty sidebar" bugs was: user opens the page right as the
    // device boots, AsyncWebServer isn't quite up yet, axios throws
    // a network error, EMPTY_MANIFEST gets committed, and the UI
    // stays broken until the operator manually signs out + in
    // (signIn dispatches reloadManifest, by which time the server
    // is ready). Backoff: 1s, 2s, 4s, 8s, 16s — five attempts cover
    // a 31-second reboot window which is plenty for the slowest
    // C3 cold-boot. Any HTTP success (including the anonymous stub)
    // counts as resolved; only true network / 5xx errors retry.
    const delays = [1000, 2000, 4000, 8000, 16000];
    for (let attempt = 0; attempt <= delays.length; attempt += 1) {
      try {
        const response = await ManifestApi.readManifest();
        const data = response.data;
        // Backend always emits valid JSON. Anonymous (stub) response
        // omits `features` — coerce to empty list so the UI renders
        // the brand without crashing on undefined; the stub still
        // carries schemaVersion + device + buildFeatures +
        // authenticated:false.
        const next: UiManifest = data
          ? {
              ...data,
              features: Array.isArray(data.features) ? data.features : [],
            }
          : EMPTY_MANIFEST;
        setManifest(next);
        applyManifest(next);
        setLoaded(true);
        setError(undefined);
        return;
      } catch (err: any) {
        const isLastAttempt = attempt === delays.length;
        const msg = extractErrorMessage(err, 'Failed to fetch UI manifest.');
        if (isLastAttempt) {
          setError(msg);
          setManifest(EMPTY_MANIFEST);
          setLoaded(true);
          return;
        }
        // Surface progress in the ManifestProgress overlay so the
        // operator sees "retrying…" rather than a frozen carousel.
        setError(`${msg} — retry ${attempt + 1}/${delays.length}`);
        await new Promise((resolve) => setTimeout(resolve, delays[attempt]));
      }
    }
  }, []);

  useEffect(() => {
    loadManifest();
  }, [loadManifest]);

  // Push the device-name (set in ESPRack::Builder("ProjectName", ...))
  // into the browser tab title every time the manifest reloads. The
  // static <title>…</title> in public/index.html is the bootstrap
  // fallback shown for the ~100 ms before React hydrates and the very
  // first manifest fetch resolves.
  useEffect(() => {
    const name = manifest.device?.name;
    if (name) {
      document.title = name;
    }
  }, [manifest]);

  const findFeature = useCallback(
    (id: string): FeatureEntry | undefined => manifest.features.find((f) => f.id === id),
    [manifest]
  );

  const value: ManifestContextValue = useMemo(
    () => ({ manifest, loaded, error, findFeature, reload: loadManifest }),
    [manifest, loaded, error, findFeature, loadManifest]
  );

  return (
    <ManifestContext.Provider value={value}>
      {revealComplete && props.children}
      {!revealComplete && (
        <ManifestProgress
          loaded={loaded}
          manifest={manifest}
          error={error}
          onRevealComplete={() => setRevealComplete(true)}
        />
      )}
    </ManifestContext.Provider>
  );
};

export default ManifestLoader;
