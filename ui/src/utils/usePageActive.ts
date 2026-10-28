// src/utils/usePageActive.ts
//
// false once the tab has stayed hidden for graceMs; true again the moment it
// is shown. The live sockets (WsProvider, PresenceProvider, useWs) close while
// it is false, so tabs left open in the background hold no connection on the
// device. A short switch away keeps the socket: no reconnect on every glance.
// A tab opened in the background starts inactive: it connects when shown.

import { useEffect, useState } from 'react';

export const PAGE_IDLE_MS = 30_000;

export const usePageActive = (graceMs: number = PAGE_IDLE_MS): boolean => {
  const [active, setActive] = useState<boolean>(() => !document.hidden);

  useEffect(() => {
    let timer: number | undefined;
    const onChange = () => {
      window.clearTimeout(timer);
      timer = undefined;
      if (document.hidden) timer = window.setTimeout(() => setActive(false), graceMs);
      else setActive(true);
    };
    document.addEventListener('visibilitychange', onChange);
    return () => {
      document.removeEventListener('visibilitychange', onChange);
      window.clearTimeout(timer);
    };
  }, [graceMs]);

  return active;
};

export default usePageActive;
