import React from 'react';

import { FeatureEntry, UiManifest } from '../../types';

export interface ManifestContextValue {
  manifest: UiManifest;
  loaded: boolean;
  error?: string;
  findFeature: (id: string) => FeatureEntry | undefined;
  // Re-fetch /api/v1/ui/manifest. Used by Authentication.signIn() after
  // the JWT lands so the stub manifest (anonymous response containing
  // brand only) gets swapped for the full authenticated map.
  reload: () => Promise<void>;
  // fetch a feature's own actions once; true once they're in. With
  // missingRef (an action a form names but the list lacks, e.g. after a
  // firmware update with the page still open): fetch the list once more.
  ensureActions: (featureId: string, missingRef?: string) => void;
  actionsLoaded: (featureId: string, missingRef?: string) => boolean;
}

const EMPTY_MANIFEST: UiManifest = {
  schemaVersion: 0,
  features: []
};

const ManifestContextDefaultValue: ManifestContextValue = {
  manifest: EMPTY_MANIFEST,
  loaded: false,
  findFeature: () => undefined,
  reload: async () => {},
  ensureActions: () => {},
  actionsLoaded: () => true
};

export const ManifestContext = React.createContext<ManifestContextValue>(ManifestContextDefaultValue);

export function useManifest(): ManifestContextValue {
  return React.useContext(ManifestContext);
}
