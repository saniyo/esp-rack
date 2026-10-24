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
// Wire envelope (matches WsManager.h):
//   client->server: {type:'sub',ch} / {type:'unsub',ch} / {type:'p',ch,origin_id,p}
//   server->client: {type:'id',id} (once) / {type:'p',ch,origin_id,p}
//                   {type:'hb'} every WS_HEARTBEAT_MS (5 s) on newer firmware

import { createContext, FC, ReactNode, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import Sockette from 'sockette';

import { WEB_SOCKET_ROOT } from '../../api/endpoints';
import { addAccessTokenParameter } from '../../api/authentication';

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

export const WsProvider: FC<{ children: ReactNode }> = ({ children }) => {
  const ws = useRef<Sockette>();
  const subs = useRef<Map<string, Set<ChannelHandler>>>(new Map());
  const lastMessageAt = useRef<number>(0);
  const heartbeatSeen = useRef<boolean>(false);
  const [connected, setConnected] = useState<boolean>(false);

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
    [sendRaw]
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

  const forceReconnect = useCallback(() => {
    const inst = ws.current;
    if (!inst) return;
    try {
      inst.reconnect();
    } catch {
      inst.close();
    }
  }, []);

  // Open the single socket once, for the provider's lifetime.
  useEffect(() => {
    const instance = new Sockette(addAccessTokenParameter(WS_APP_URL), {
      onmessage: onMessage,
      onopen: () => {
        setConnected(true);
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
      onclose: () => {
        setConnected(false);
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
      instance.close();
      ws.current = undefined;
    };
  }, [onMessage, sendRaw]);

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
        forceReconnect();
      }
    }, 5000);
    return () => window.clearInterval(id);
  }, [connected, forceReconnect]);

  // Reconnect proactively when the tab regains focus or the OS network resumes.
  useEffect(() => {
    const onVisible = () => {
      if (document.visibilityState === 'visible' && !connected) forceReconnect();
    };
    const onOnline = () => forceReconnect();
    document.addEventListener('visibilitychange', onVisible);
    window.addEventListener('online', onOnline);
    return () => {
      document.removeEventListener('visibilitychange', onVisible);
      window.removeEventListener('online', onOnline);
    };
  }, [connected, forceReconnect]);

  const value = useMemo<WsContextValue>(
    () => ({ connected, subscribe, send }),
    [connected, subscribe, send]
  );

  return <WsContext.Provider value={value}>{children}</WsContext.Provider>;
};

export default WsProvider;
