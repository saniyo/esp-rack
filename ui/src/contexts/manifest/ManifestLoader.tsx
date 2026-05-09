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
    try {
      const response = await ManifestApi.readManifest();
      const data = response.data;
      // Backend always emits valid JSON. Anonymous (stub) response
      // omits `features` — coerce to empty list so the UI renders the
      // brand without crashing on undefined; the stub still carries
      // schemaVersion + device + buildFeatures + authenticated:false.
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
    } catch (err: any) {
      setError(extractErrorMessage(err, 'Failed to fetch UI manifest.'));
      setManifest(EMPTY_MANIFEST);
      setLoaded(true);
    }
  }, []);

  useEffect(() => {
    loadManifest();
  }, [loadManifest]);

  // Sync the browser tab title to the canonical project name from the
  // manifest. Falls back to "ESPRack" only when the manifest hasn't
  // landed yet (race window) or the device record is missing. The
  // static index.html title shows briefly during the very first paint
  // before this fires; everything after the first /rest/uiManifest
  // response shows the real project name (Builder("...") first arg).
  useEffect(() => {
    const name = manifest.device?.name;
    if (name) document.title = name;
  }, [manifest.device?.name]);

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
