// src/contexts/ws/WsProvider.tsx
//
// ONE app-level WebSocket for the whole UI. All feature live-state flows over a
// single /ws/app connection, multiplexed by a `ch` (channel = feature id) field.
// This is the single conduit: any component subscribes to the channels it needs
// via useWsChannel(); the provider owns the socket, routes inbound frames to the
// right subscribers, and re-subscribes everything on reconnect.
//
// Presence (/ws/presence) stays a SEPARATE always-on socket by design — its value
// is independence. High-rate/binary streams must also stay separate (head-of-line
// blocking on one ordered stream).
//
// A tab hidden for PAGE_IDLE_MS closes the socket (paused) and opens a fresh
// one when shown again: onopen resubscribes, so every channel gets its state
// snapshot at once. Background tabs hold no client on the device.
//
// Sockette's reconnect() on an open or connecting socket opens a second
// WebSocket and leaves the first one running (still subscribed, never
// closed): a client leaked on the device each time. A fresh socket is made
// instead (restart): the old instance is closed first.
//
// The device caps its clients (WsManager.h). It closes a hidden tab first, so
// the page tells it whether it is shown ({type:'vis'}, on open and on every
// change); with every other client in view the newcomer is refused. Either
// way the close is 1000 "busy" / "evicted", which Sockette does not retry:
// the tab shows Busy and tries again when it is shown, used or moves to
// another page, and on its own after 5 s, 10 s, 20 s ... up to BUSY_RETRY_MS.
// The URL names the tab (?tab=, new per page load), so the device drops this
// tab's own older socket, e.g. one a Wi-Fi drop left behind, instead of
// counting it against the newcomer.
//
// Wire envelope (matches WsManager.h):
//   client->server: {type:'sub',ch} / {type:'unsub',ch} / {type:'p',ch,origin_id,p}
//                   {type:'vis',hidden} (no channel; older firmware ignores it)
//   server->client: {type:'id',id} (once) / {type:'p',ch,origin_id,p}
//                   {type:'hb'} every WS_HEARTBEAT_MS (5 s) on newer firmware

import { createContext, FC, ReactNode, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import Sockette from 'sockette';

import { WEB_SOCKET_ROOT } from '../../api/endpoints';
import { addAccessTokenParameter } from '../../api/authentication';
import { usePageActive } from '../../utils/usePageActive';

// Inbound push/snapshot frame handed to channel subscribers.
export interface WsChannelMessage {
  type: string;
  ch: string;
  origin_id?: string;
  p?: any;
}

type ChannelHandler = (msg: WsChannelMessage) => void;

export interface WsContextValue {
  connected: boolean;
  // The socket was closed because the tab sat in the background; stays set
  // after it is shown again until the fresh socket opens (or fails).
  paused: boolean;
  // The device turned the socket down or closed it for another client: it
  // has as many as it takes. Tried again when the tab is shown or used.
  busy: boolean;
  // Subscribe to a channel; returns an unsubscribe fn. Safe to call before the
  // socket is open (the sub is (re)sent on connect).
  subscribe: (channel: string, handler: ChannelHandler) => () => void;
  // Send a frame on a channel. `data` is the envelope built by the caller
  // (e.g. {type:'p',origin_id,p}); the channel id is injected here.
  send: (channel: string, data: Record<string, any>) => void;
}

export const WsContext = createContext<WsContextValue | null>(null);

// Single multiplexed socket path (distinct from /ws/presence). WEB_SOCKET_ROOT is
// e.g. "ws://host/ws/"; append "app" -> "ws://host/ws/app".
const WS_APP_URL = WEB_SOCKET_ROOT + 'app';

// This page load's tab id. Not in sessionStorage: a duplicated tab copies
// that, and two tabs with one id would close each other's socket.
const TAB_ID = Math.random().toString(36).slice(2, 10);

const appUrl = (): string => {
  const url = new URL(addAccessTokenParameter(WS_APP_URL));
  url.searchParams.set('tab', TAB_ID);
  return url.toString();
};

// See useWs.ts for the rationale behind these values.
const STALE_WATCHDOG_MS = 600_000;
// Once the device has sent a heartbeat on this connection, silence this long
// means the socket is dead or stuck (Live shown, nothing arriving): reconnect.
// Firmware without the heartbeat keeps the long watchdog above.
const HEARTBEAT_STALE_MS = 15_000;
const RECONNECT_MAX_ATTEMPTS = 10_000;
// Stagger resubscribes on reconnect so N channels don't fire N synchronous
// state snapshots on the device's single async_tcp task in one burst.
const RESUB_STAGGER_MS = 40;
// Busy: a new try at most this often on use; on its own after the first
// delay, doubled per refusal in a row, up to the last.
const BUSY_RETRY_ON_USE_MS = 5_000;
const BUSY_RETRY_FIRST_MS = 5_000;
const BUSY_RETRY_MS = 60_000;
// A socket closed as busy within this long of opening was refused (counts
// towards the backoff); after that it had been working.
const BUSY_REFUSED_WITHIN_MS = 10_000;

export const WsProvider: FC<{ children: ReactNode }> = ({ children }) => {
  const ws = useRef<Sockette>();
  const subs = useRef<Map<string, Set<ChannelHandler>>>(new Map());
  const lastMessageAt = useRef<number>(0);
  const heartbeatSeen = useRef<boolean>(false);
  const [connected, setConnected] = useState<boolean>(false);
  const active = usePageActive();
  const [paused, setPaused] = useState<boolean>(false);
  // Refusals in a row ("busy" / "evicted"); 0 when the last close was not one.
  const [busyCount, setBusyCount] = useState<number>(0);
  const openedAt = useRef<number>(0);
  const lastUseRetry = useRef<number>(0);
  // Bumped to replace the socket with a fresh one (see the header note).
  const [generation, setGeneration] = useState<number>(0);
  // For subscribe(), which keeps a stable identity.
  const live = useRef({ busy: false, connected: false });
  live.current = { busy: busyCount > 0, connected };

  const restart = useCallback(() => {
    if (ws.current) setGeneration((g) => g + 1);  // none while paused
  }, []);

  const sendRaw = useCallback((obj: Record<string, any>) => {
    const inst = ws.current;
    if (!inst) return;
    try {
      inst.json(obj);
    } catch {
      // socket not open yet — onopen resubscribes, and sends are best-effort
    }
  }, []);

  const send = useCallback(
    (channel: string, data: Record<string, any>) => {
      sendRaw({ ...data, ch: channel });
    },
    [sendRaw]
  );

  const subscribe = useCallback(
    (channel: string, handler: ChannelHandler) => {
      let set = subs.current.get(channel);
      const firstForChannel = !set || set.size === 0;
      if (!set) {
        set = new Set();
        subs.current.set(channel, set);
      }
      set.add(handler);
      // First subscriber for this channel → tell the server. If the socket
      // isn't open yet this is dropped, but onopen resubscribes every channel.
      if (firstForChannel) sendRaw({ type: 'sub', ch: channel });
      // Busy and on another page now: it may have room, try at once (shown,
      // and no more often than a retry on use).
      if (
        firstForChannel &&
        live.current.busy &&
        !live.current.connected &&
        !document.hidden &&
        Date.now() - lastUseRetry.current >= BUSY_RETRY_ON_USE_MS
      ) {
        lastUseRetry.current = Date.now();
        restart();
      }

      return () => {
        const s = subs.current.get(channel);
        if (!s) return;
        s.delete(handler);
        if (s.size === 0) {
          subs.current.delete(channel);
          sendRaw({ type: 'unsub', ch: channel });
        }
      };
    },
    [sendRaw, restart]
  );

  const onMessage = useCallback((event: MessageEvent) => {
    const raw = event.data;
    if (typeof raw !== 'string') return;
    lastMessageAt.current = Date.now();
    let msg: WsChannelMessage;
    try {
      msg = JSON.parse(raw) as WsChannelMessage;
    } catch (e) {
      console.error('[ws] bad frame', e);
      return;
    }
    if (msg && msg.type === 'hb') {
      heartbeatSeen.current = true;
      return;
    }
    // {type:'id',...} connection-identity frame has no channel — ignore.
    if (!msg || !msg.ch) return;
    const set = subs.current.get(msg.ch);
    if (set) set.forEach((h) => h(msg));
  }, []);

  // The single socket, while the tab is in use; a new one per generation.
  useEffect(() => {
    if (!active) {
      setPaused(true);
      return;
    }
    const instance = new Sockette(appUrl(), {
      onmessage: onMessage,
      onopen: () => {
        setConnected(true);
        setPaused(false);
        openedAt.current = Date.now();
        sendRaw({ type: 'vis', hidden: document.hidden });  // before the subs: the device caps by it
        lastMessageAt.current = Date.now();
        heartbeatSeen.current = false;
        // Resubscribe every active channel, staggered.
        let i = 0;
        subs.current.forEach((_set, channel) => {
          const delay = i++ * RESUB_STAGGER_MS;
          window.setTimeout(() => sendRaw({ type: 'sub', ch: channel }), delay);
        });
        console.log('[ws] /ws/app connected');
      },
      onclose: (e: CloseEvent) => {
        if (ws.current !== instance) return;  // a closed, replaced one
        setConnected(false);
        setPaused(false);
        const refused = !!e && e.code === 1000 && (e.reason === 'busy' || e.reason === 'evicted');
        const quick = Date.now() - openedAt.current < BUSY_REFUSED_WITHIN_MS;
        setBusyCount((n) => (refused ? (quick ? n : 0) + 1 : 0));
        console.warn('[ws] /ws/app disconnected');
      },
      onerror: (error) => {
        console.error('[ws] error', error);
      },
      timeout: 5000,
      maxAttempts: RECONNECT_MAX_ATTEMPTS,
    });
    ws.current = instance;
    return () => {
      ws.current = undefined;
      instance.close();
      setConnected(false);
    };
  }, [active, generation, onMessage, sendRaw]);

  // Staleness watchdog — covers the "socket says connected but device stopped
  // pushing" class. lastMessageAt bumps on every inbound frame.
  useEffect(() => {
    const id = window.setInterval(() => {
      if (!connected) return;
      if (lastMessageAt.current === 0) return;
      const stale = Date.now() - lastMessageAt.current;
      if (stale > (heartbeatSeen.current ? HEARTBEAT_STALE_MS : STALE_WATCHDOG_MS)) {
        console.warn(`[ws] stale (${stale} ms) — forcing reconnect`);
        lastMessageAt.current = 0;
        restart();
      }
    }, 5000);
    return () => window.clearInterval(id);
  }, [connected, restart]);

  // Tell the device on every change (it closes hidden tabs first). Reconnect
  // at once (not on the 5 s retry) when the tab is shown with the socket
  // down, or the OS network resumes. Shown after a pause: the socket effect
  // opens a new one anyway (restart() is a no-op without a socket).
  useEffect(() => {
    const onVisible = () => {
      if (connected) sendRaw({ type: 'vis', hidden: document.hidden });
      if (document.visibilityState === 'visible' && !connected) restart();
    };
    const onOnline = () => restart();
    document.addEventListener('visibilitychange', onVisible);
    window.addEventListener('online', onOnline);
    return () => {
      document.removeEventListener('visibilitychange', onVisible);
      window.removeEventListener('online', onOnline);
    };
  }, [connected, restart, sendRaw]);

  // Busy: try again when the tab is used (at most every
  // BUSY_RETRY_ON_USE_MS), and on its own with a backoff while it is shown.
  useEffect(() => {
    if (!busyCount || !active || connected) return;
    const delay = Math.min(BUSY_RETRY_FIRST_MS * 2 ** (busyCount - 1), BUSY_RETRY_MS);
    const timer = window.setTimeout(() => {
      if (!document.hidden) restart();
    }, delay);
    const onUse = () => {
      if (document.hidden || Date.now() - lastUseRetry.current < BUSY_RETRY_ON_USE_MS) return;
      lastUseRetry.current = Date.now();
      restart();
    };
    window.addEventListener('focus', onUse);
    window.addEventListener('pointerdown', onUse);
    window.addEventListener('keydown', onUse);
    return () => {
      window.clearTimeout(timer);
      window.removeEventListener('focus', onUse);
      window.removeEventListener('pointerdown', onUse);
      window.removeEventListener('keydown', onUse);
    };
  }, [busyCount, active, connected, restart]);

  const busy = busyCount > 0 && !connected;
  const value = useMemo<WsContextValue>(
    () => ({ connected, paused, busy, subscribe, send }),
    [connected, paused, busy, subscribe, send]
  );

  return <WsContext.Provider value={value}>{children}</WsContext.Provider>;
};

export default WsProvider;
