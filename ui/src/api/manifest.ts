import { AxiosPromise } from 'axios';

import { UiManifest } from '../types';
import { AXIOS } from './endpoints';

export const UI_MANIFEST_PATH = '/uiManifest';

export function readManifest(): AxiosPromise<UiManifest> {
  return AXIOS.get(UI_MANIFEST_PATH);
}
