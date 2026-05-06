import { FC, useEffect, useRef } from 'react';
import { useLocation } from 'react-router-dom';
import Sockette from 'sockette';

import { getClientKey } from '../../utils/clientKey';
import { WEB_SOCKET_ROOT } from '../../api/endpoints';
import { addAccessTokenParameter } from '../../api/authentication';
import { RequiredChildrenProps } from '../../utils';

// Global presence channel — one persistent /ws/presence connection per
// authenticated SPA session. Survives feature navigation (any feature's
// own WS comes and goes independently), so the backend always knows which
// tab is open and where the user currently is.
//
// Frames:
//   hello: { type: "hello", clientKey, page, userAgent }
//   page : { type: "page",  page }
const PRESENCE_WS_PATH = 'presence';

export const PresenceProvider: FC<RequiredChildrenProps> = ({ children }) => {
  const location = useLocation();
  const wsRef = useRef<Sockette | null>(null);
  const connectedRef = useRef(false);
  const pendingPageRef = useRef<string | null>(null);

  // Open the WS once at mount, close on unmount. We deliberately DO NOT
  // restart on route changes — the whole point is that this channel is
  // stable across the session.
  useEffect(() => {
    const clientKey = getClientKey();
    const wsUrl = addAccessTokenParameter(WEB_SOCKET_ROOT + PRESENCE_WS_PATH);

    const sendHello = (s: Sockette) => {
      s.json({
        type: 'hello',
        clientKey,
        page: window.location.pathname + window.location.search,
        userAgent: navigator.userAgent,
      });
    };

    const s = new Sockette(wsUrl, {
      onopen: () => {
        connectedRef.current = true;
        sendHello(s);
        // Flush any page change that happened while we were disconnected.
        if (pendingPageRef.current) {
          s.json({ type: 'page', page: pendingPageRef.current });
          pendingPageRef.current = null;
        }
      },
      onclose: () => {
        connectedRef.current = false;
      },
      onerror: () => {
        // silent — Sockette will auto-reconnect
      },
      timeout: 5000,
      maxAttempts: 20,
    });

    wsRef.current = s;
    return () => {
      s.close();
      wsRef.current = null;
      connectedRef.current = false;
    };
  }, []);

  // Push page-change notifications as the user navigates. If WS is briefly
  // offline, stash the latest path; the onopen flush above sends it once
  // reconnection succeeds.
  useEffect(() => {
    const page = location.pathname + location.search;
    const ws = wsRef.current;
    if (ws && connectedRef.current) {
      ws.json({ type: 'page', page });
    } else {
      pendingPageRef.current = page;
    }
  }, [location.pathname, location.search]);

  return <>{children}</>;
};

export default PresenceProvider;
