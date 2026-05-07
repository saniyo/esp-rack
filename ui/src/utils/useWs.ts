// src/utils/useWs.ts

import { useCallback, useEffect, useRef, useState } from 'react';
import Sockette from 'sockette';
import { throttle } from 'lodash';
import { addAccessTokenParameter } from '../api/authentication';

export interface WebSocketIdMessage {
  type: 'id';
  id: string;
  origin_id: string;
}

export interface WebSocketPayloadMessage<D> {
  type: 'p';
  origin_id: string;
  p: D;
}

export type WebSocketMessage<D> = WebSocketIdMessage | WebSocketPayloadMessage<D>;

// How long to wait for a payload message on a `live=true` tab before
// declaring the connection stale and forcing a reconnect. Originally
// 30s, which false-triggered relentlessly on event-driven tabs
// (Telegram pushes only on send/queue change; MQTT only on broker
// state flip; NTP on clock refresh) — quiet periods are NORMAL, the
// watchdog kept reconnecting against a healthy socket and slot-leaking
// the AsyncWebSocket pool until the server refused new clients (then
// even a hard browser refresh couldn't get a Live indicator back).
//
// The browser's own onclose path already detects truly dead sockets
// via TCP keepalive + AsyncWebSocket's beginPingPong (20s ping / 60s
// pong timeout). The watchdog only adds value for the rare server-
// side deadlock where the task is hung but TCP is still alive — a
// 10-minute window catches that without firing on any normal idle
// pattern. Light's 10Hz chart still trips the alarm in ~6000 missed
// frames if it ever genuinely freezes.
const STALE_WATCHDOG_MS = 600_000;

// Sockette retry cap. The legacy 10 was reached within ~50 s and then
// the socket died for the lifetime of the page; long-running operator
// sessions hit this routinely after a Wi-Fi blip / device reboot. With
// Sockette's exponential backoff capped at ~30 s, 10000 attempts means
// "effectively forever" — the user can always take the tab offline by
// closing it.
const RECONNECT_MAX_ATTEMPTS = 10_000;

export const useWs = <D>(
  wsUrl: string,
  wsThrottle: number = 500,
  maxAttempts: number = RECONNECT_MAX_ATTEMPTS
) => {
  const ws = useRef<Sockette>();
  const clientId = useRef<string>();
  const lastMessageAt = useRef<number>(0);

  const [connected, setConnected] = useState<boolean>(false);
  const [originId, setOriginId] = useState<string>('');
  const [wsData, setWsData] = useState<D | undefined>();
  // Bumps on every inbound frame. Consumers (LiveIndicator) can use
  // it as an animation trigger — re-keying off the counter restarts a
  // one-shot pulse so the dot blinks per real heartbeat instead of
  // pulsing constantly while connected. Bounded by uint counter
  // semantics; React's state diff is value-based so wraparound is fine.
  const [messageTick, setMessageTick] = useState<number>(0);

  // Outgoing-state coordination flags (see updateData).
  const [transmit, setTransmit] = useState<boolean>(false);
  const [clear, setClear] = useState<boolean>(false);

  const onMessage = useCallback((event: MessageEvent) => {
    const rawData = event.data;
    if (typeof rawData === 'string') {
      try {
        const message = JSON.parse(rawData) as WebSocketMessage<D>;
        // Stamp on EVERY message including 'id' frames so the watchdog
        // doesn't false-trigger between connect and the first 'p'.
        lastMessageAt.current = Date.now();
        // Heartbeat tick — bumped on every inbound frame regardless of
        // type so any subscribed UI element (currently LiveIndicator)
        // can re-key its animation off it. Counter value itself is
        // not meaningful, only the change triggers re-render.
        setMessageTick((n) => (n + 1) | 0);
        switch (message.type) {
          case 'id':
            clientId.current = message.id;
            setOriginId(message.origin_id);
            break;
          case 'p':
            if (message.origin_id) {
              setOriginId(message.origin_id);
            }
            setWsData(message.p);
            break;
          default:
            console.warn(`[useWs] Unknown message type: ${message}`);
        }
      } catch (error) {
        console.error('[useWs] Error parsing message:', error);
      }
    }
  }, []);

  const doSaveData = useCallback(
    (newData: D, clearData: boolean = false) => {
      if (!ws.current) return;
      if (clearData) {
        setWsData(undefined);
      }
      ws.current.json(newData);
    },
    []
  );

  const saveData = useRef(throttle(doSaveData, wsThrottle));

  const updateData = (
    newData: React.SetStateAction<D | undefined>,
    transmitData: boolean = true,
    clearData: boolean = false
  ) => {
    setWsData(newData);
    setTransmit(transmitData);
    setClear(clearData);
  };

  const disconnect = useCallback(() => {
    if (ws.current) {
      ws.current.close();
    }
  }, []);

  // Force a fresh socket — tears down the current Sockette instance and
  // lets the connect-effect re-run (we re-use the existing wsUrl deps so
  // bumping a nonce here would be redundant; calling reconnect() drops
  // the underlying WebSocket which fires onclose → Sockette's own
  // reconnect kicks immediately, sidestepping the long timeout we'd
  // otherwise wait through).
  const forceReconnect = useCallback(() => {
    const inst = ws.current;
    if (!inst) return;
    try {
      inst.reconnect();
    } catch {
      // Older Sockette versions: fall back to close → instance reopens
      // via its own retry loop.
      inst.close();
    }
  }, []);

  useEffect(() => {
    if (!transmit) return;
    if (wsData) {
      saveData.current(wsData, clear);
    }
    setTransmit(false);
    setClear(false);
  }, [wsData, transmit, clear]);

  // Connect + register handlers. Empty wsUrl = noop (DynamicFeature
  // calls useWs unconditionally and passes '' for non-live features).
  useEffect(() => {
    if (!wsUrl) return;

    let attempts = 0;

    const instance = new Sockette(addAccessTokenParameter(wsUrl), {
      onmessage: onMessage,
      onopen: () => {
        setConnected(true);
        attempts = 0;
        lastMessageAt.current = Date.now();
        console.log('[useWs] WebSocket connected');
      },
      onclose: () => {
        clientId.current = undefined;
        setConnected(false);
        setWsData(undefined);
        console.warn('[useWs] WebSocket disconnected');
        attempts++;
        if (attempts > maxAttempts) {
          console.warn('[useWs] Stop attempts! Reached max attempts:', attempts);
        } else {
          console.log(`[useWs] Attempt ${attempts} to reconnect…`);
        }
      },
      onerror: (error) => {
        console.error('[useWs] WebSocket error:', error);
      },
      timeout: 5000,
      maxAttempts: maxAttempts,
    });

    ws.current = instance;
    return () => {
      instance.close();
    };
  }, [wsUrl, onMessage, maxAttempts]);

  // Staleness watchdog — covers the "page hung on the live tab forever"
  // class of bugs where the TCP socket reports `connected=true` but the
  // device stopped pushing (typical: deep-sleep / brown-out / AsyncWS
  // backpressure that silently dropped this client). lastMessageAt is
  // updated on every inbound frame; if we go past the threshold we
  // force-reconnect. Disabled when wsUrl is empty.
  useEffect(() => {
    if (!wsUrl) return;
    const id = window.setInterval(() => {
      if (!connected) return;                    // already trying to reconnect
      if (lastMessageAt.current === 0) return;   // never received a frame yet
      const stale = Date.now() - lastMessageAt.current;
      if (stale > STALE_WATCHDOG_MS) {
        console.warn(`[useWs] Stale WS (${stale} ms since last msg) — forcing reconnect`);
        lastMessageAt.current = 0;               // reset so next interval doesn't fire again
        forceReconnect();
      }
    }, 5000);
    return () => window.clearInterval(id);
  }, [wsUrl, connected, forceReconnect]);

  // Visibility + network resume handlers. Tabs in the background pause
  // their WS internally on some browsers; coming back to the foreground
  // after long absence used to leave the socket in a half-broken state
  // until the user manually refreshed. Here we proactively reconnect on
  // both visibilitychange (tab focus regained) and `online` (OS-level
  // network came back) so the recovery is automatic.
  useEffect(() => {
    if (!wsUrl) return;
    const onVisible = () => {
      if (document.visibilityState === 'visible' && !connected) {
        console.log('[useWs] Tab became visible & WS down → reconnect');
        forceReconnect();
      }
    };
    const onOnline = () => {
      console.log('[useWs] Network back online → reconnect');
      forceReconnect();
    };
    document.addEventListener('visibilitychange', onVisible);
    window.addEventListener('online', onOnline);
    return () => {
      document.removeEventListener('visibilitychange', onVisible);
      window.removeEventListener('online', onOnline);
    };
  }, [wsUrl, connected, forceReconnect]);

  return {
    connected,
    originId,
    wsData,
    updateData,
    disconnect,
    forceReconnect,
    messageTick,
  } as const;
};
