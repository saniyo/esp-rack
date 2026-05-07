import React from 'react';

import { FeatureEntry, UiManifest } from '../../types';

export interface ManifestContextValue {
  manifest: UiManifest;
  loaded: boolean;
  error?: string;
  findFeature: (id: string) => FeatureEntry | undefined;
  // Re-fetch /rest/uiManifest. Used by Authentication.signIn() after
  // the JWT lands so the stub manifest (anonymous response containing
  // brand only) gets swapped for the full authenticated map.
  reload: () => Promise<void>;
}

const EMPTY_MANIFEST: UiManifest = {
  schemaVersion: 0,
  features: []
};

const ManifestContextDefaultValue: ManifestContextValue = {
  manifest: EMPTY_MANIFEST,
  loaded: false,
  findFeature: () => undefined,
  reload: async () => {}
};

export const ManifestContext = React.createContext<ManifestContextValue>(ManifestContextDefaultValue);

export function useManifest(): ManifestContextValue {
  return React.useContext(ManifestContext);
}
