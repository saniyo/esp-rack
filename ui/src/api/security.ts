import { AxiosPromise } from 'axios';

import { SecuritySettings } from '../types';
import { AXIOS } from './endpoints';

// The `security` typed feature (Admin) — registry-derived C1 state face.
export function readSecuritySettings(): AxiosPromise<SecuritySettings> {
  return AXIOS.get('security/state');
}

export function updateSecuritySettings(securitySettings: SecuritySettings): AxiosPromise<SecuritySettings> {
  return AXIOS.post('security/state', securitySettings);
}
