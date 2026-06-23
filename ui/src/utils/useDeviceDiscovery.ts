import { useEffect, useState } from 'react';
import { AXIOS } from '../api/endpoints';

// A single LAN peer as reported by the discovery module's GET /api/v1/discovery/peers.
export interface Peer {
  device_id: string;
  project: string;
  fw: string;
  schema: string;
  framework: string;
  ip: string;
  port: number;
  trusted: boolean;     // project-key (PSK) HMAC verified
  compatible: boolean;  // manifest schema this device can render
  age_ms: number;
}

export interface SelfInfo {
  device_id: string;
  project: string;
  fw: string;
  schema: string;
  framework: string;
  ip: string;
}

export interface DeviceDiscovery {
  self: SelfInfo | null;
  peers: Peer[];
}

// useDeviceDiscovery — polls GET /api/v1/discovery/peers (the discovery
// module's discovered, trust-verified peer list + this device's own info).
// Returns empty/null GRACEFULLY when the discovery module isn't installed (the
// endpoint 404s), so the menu degrades to the classic single-device layout.
// One shared fetch path for both the menu (peers) and the Discovery page
// (self + peers).
//
// C3-friendly: it does NOT poll while the tab is hidden (no background tab
// hammering the device's tiny connection budget), and a transient failure
// (device momentarily busy) KEEPS the last roster instead of wiping it.
export function useDeviceDiscovery(pollMs = 30000): DeviceDiscovery {
  const [data, setData] = useState<DeviceDiscovery>({ self: null, peers: [] });

  useEffect(() => {
    let alive = true;
    let timer: ReturnType<typeof setInterval> | undefined;

    const load = () => {
      if (document.hidden) return; // don't poll a backgrounded tab
      AXIOS.get('discovery/peers')
        .then((r) => {
          if (!alive) return;
          setData({
            self: r.data?.self ?? null,
            peers: Array.isArray(r.data?.peers) ? r.data.peers : [],
          });
        })
        .catch(() => {
          /* transient (e.g. device busy) — keep the last roster, don't wipe */
        });
    };

    const start = () => {
      if (timer) return;
      load();
      timer = setInterval(load, pollMs);
    };
    const stop = () => {
      if (timer) {
        clearInterval(timer);
        timer = undefined;
      }
    };
    const onVisibility = () => (document.hidden ? stop() : start());

    if (!document.hidden) start();
    document.addEventListener('visibilitychange', onVisibility);
    return () => {
      alive = false;
      stop();
      document.removeEventListener('visibilitychange', onVisibility);
    };
  }, [pollMs]);

  return data;
}
