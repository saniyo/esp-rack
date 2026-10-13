import { useMemo } from 'react';

import { useManifest } from '../contexts/manifest';
import { resolveZone, ResolvedZone } from './deviceTime';

// The device's timezone from the manifest (device.tz), resolved for Intl.
export function useDeviceZone(): ResolvedZone {
  const { manifest } = useManifest();
  const iana = manifest.device?.tz?.iana;
  const posix = manifest.device?.tz?.posix;
  return useMemo(() => resolveZone({ iana, posix }), [iana, posix]);
}
