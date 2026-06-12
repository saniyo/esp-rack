import { AxiosPromise } from 'axios';

import { UiManifest } from '../types';
import { AXIOS } from './endpoints';

// Base-relative: AXIOS baseURL is '/api/v1/', so resolves to /api/v1/ui/manifest.
export const UI_MANIFEST_PATH = 'ui/manifest';

export function readManifest(): AxiosPromise<UiManifest> {
  return AXIOS.get(UI_MANIFEST_PATH);
}
