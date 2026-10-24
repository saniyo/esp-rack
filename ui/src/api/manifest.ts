import { AxiosPromise } from 'axios';

import { FeatureEntry, UiManifest } from '../types';
import { AXIOS } from './endpoints';

// Base-relative: AXIOS baseURL is '/api/v1/', so resolves to /api/v1/ui/manifest.
export const UI_MANIFEST_PATH = 'ui/manifest';

export function readManifest(): AxiosPromise<UiManifest> {
  return AXIOS.get(UI_MANIFEST_PATH);
}

// a feature's own actions, which the cached manifest leaves out.
export const UI_ACTIONS_PATH = 'ui/actions';

export function readFeatureActions(featureId: string): AxiosPromise<{ actions?: FeatureEntry[] }> {
  return AXIOS.get(UI_ACTIONS_PATH, { params: { of: featureId } });
}

// the device's runtime facts (its time zone), not in the manifest.
export const UI_DEVICE_PATH = 'ui/device';

export function readDeviceInfo(): AxiosPromise<{ tz?: { iana?: string; posix?: string } }> {
  return AXIOS.get(UI_DEVICE_PATH);
}
