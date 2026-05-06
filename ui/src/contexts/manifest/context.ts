import React from 'react';

import { FeatureEntry, UiManifest } from '../../types';

export interface ManifestContextValue {
  manifest: UiManifest;
  loaded: boolean;
  error?: string;
  findFeature: (id: string) => FeatureEntry | undefined;
}

const EMPTY_MANIFEST: UiManifest = {
  schemaVersion: 0,
  features: []
};

const ManifestContextDefaultValue: ManifestContextValue = {
  manifest: EMPTY_MANIFEST,
  loaded: false,
  findFeature: () => undefined
};

export const ManifestContext = React.createContext<ManifestContextValue>(ManifestContextDefaultValue);

export function useManifest(): ManifestContextValue {
  return React.useContext(ManifestContext);
}
