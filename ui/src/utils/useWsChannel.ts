// src/utils/useWsChannel.ts
//
// Per-channel view onto the single app WebSocket (WsProvider). Subscribes on
// mount, unsubscribes on unmount, and returns the SAME shape the old per-feature
// useWs() returned — so DynamicFeature / DynamicStatus consume it unchanged.
//
// Echo-suppression stays per-channel: each channel has its own wsData/originId,
// so a write on channel A can never suppress a real push on channel B.
//
// ── Inbound render coalescing (the "slideshow" fix) ───────────────────────────
// The device pushes live state at up to 100 Hz (lightState tick_ms, default
// 10 Hz). Re-rendering the whole DynamicStatus form + its Recharts trends on
// every single frame backs the main thread up into a juddering slideshow: React
// renders queue faster than the browser can paint. So we DECOUPLE render rate
// from data rate — buffer frames in a ref and flush them on requestAnimationFrame.
// React then re-renders at most once per paint and self-throttles when the
// machine can't keep up (frames coalesce into fewer, fuller renders) instead of
// falling behind. Array payloads that are increments (trend point batches,
// appended or upserted table rows) are CONCATENATED across coalesced frames so
// no live data is dropped; arrays a consumer registered as full sets (category
// trends, replace-mode tables: registerReplaceKeys) and scalar fields are
// last-write-wins (the newest snapshot is the truth). rAF also pauses while
// the tab is hidden — no wasted renders in the background.
//
// Outbound writes are merged per key and sent at most every wsThrottle ms, so
// fields edited in quick succession all go out; while the socket is down they
// wait and go out when it is back.

import { useCallback, useContext, useEffect, useRef, useState } from 'react';
import { throttle } from 'lodash';

import { WsContext, WsChannelMessage } from '../contexts/ws/WsProvider';

// Bound any single accumulated array while coalescing. A backgrounded tab pauses
// rAF, so frames pile into the buffer at the push rate until it foregrounds —
// cap each array so that buffer can't grow unbounded. Downstream trend/table
// merges re-cap to their own maxPoints/maxRows anyway; this only bounds the
// transient coalesce buffer (kept generous so no on-screen point is lost).
const COALESCE_ARRAY_CAP = 300;

// Merge a freshly-arrived payload `next` into the pending accumulator `acc`.
// Arrays concat (keep newest within the cap) so coalesced frames lose no live
// data, unless `isFullSet(key)`: then, like everything else, the newest wins.
export const coalescePayload = (acc: any, next: any, isFullSet: (key: string) => boolean = () => false): any => {
  if (next === undefined || next === null) return acc;
  if (typeof next !== 'object') return next;
  if (acc === undefined || acc === null || typeof acc !== 'object') {
    // Seed from a shallow copy so we never mutate the caller's frame object.
    return Array.isArray(next) ? next.slice() : { ...next };
  }
  const out: any = Array.isArray(acc) ? acc.slice() : { ...acc };
  for (const k of Object.keys(next)) {
    const nv = (next as any)[k];
    const av = (out as any)[k];
    if (Array.isArray(nv) && Array.isArray(av) && !isFullSet(k)) {
      const merged = av.concat(nv);
      out[k] =
        merged.length > COALESCE_ARRAY_CAP
          ? merged.slice(merged.length - COALESCE_ARRAY_CAP)
          : merged;
    } else {
      out[k] = nv;
    }
  }
  return out;
};

// Merge an outbound write into the one waiting: the {p: {...}} dicts merge
// per key, the rest of the envelope is the newest.
export const mergeOutbound = (acc: any, next: any): any => {
  if (acc && next && acc.p && next.p && typeof acc.p === 'object' && typeof next.p === 'object') {
    return { ...next, p: { ...acc.p, ...next.p } };
  }
  return next;
};

export const useWsChannel = <D>(channel?: string, wsThrottle: number = 500) => {
  const ctx = useContext(WsContext);

  const [connected, setConnected] = useState<boolean>(false);
  const [wsData, setWsData] = useState<D | undefined>();
  const [originId, setOriginId] = useState<string>('');
  // Bumps once per coalesced flush (not per raw frame) — drives LiveIndicator's
  // pulse. Slightly fewer pulses under heavy load, which is the desired effect.
  const [messageTick, setMessageTick] = useState<number>(0);

  // Outgoing-state coordination flags (mirror the old useWs).
  const [transmit, setTransmit] = useState<boolean>(false);
  const [clear, setClear] = useState<boolean>(false);

  // Stable send path for the throttle (channel/ctx read via refs so the
  // throttled fn identity never changes — matches useWs's doSaveData pattern).
  const chRef = useRef<string | undefined>(channel);
  chRef.current = channel;
  const ctxRef = useRef(ctx);
  ctxRef.current = ctx;

  // Writes waiting to go out, merged per key (mergeOutbound).
  const outRef = useRef<any>(undefined);
  const flushOut = useCallback(() => {
    const ch = chRef.current;
    const c = ctxRef.current;
    if (!ch || !c || outRef.current === undefined) return;
    if (!c.connected) return;  // kept: sent when the socket is back
    const data = outRef.current;
    outRef.current = undefined;
    c.send(ch, data);
  }, []);
  const saveData = useRef(throttle(flushOut, wsThrottle));

  // Keys whose arrays are full sets on every push (see coalescePayload), with
  // a count per registration so several consumers of a channel can share it.
  const fullSetKeys = useRef<Map<string, number>>(new Map());
  const isFullSet = useCallback((key: string) => fullSetKeys.current.has(key), []);
  const registerReplaceKeys = useCallback((keys: string[]) => {
    const m = fullSetKeys.current;
    keys.forEach((k) => m.set(k, (m.get(k) || 0) + 1));
    return () => {
      keys.forEach((k) => {
        const n = (m.get(k) || 0) - 1;
        if (n > 0) m.set(k, n);
        else m.delete(k);
      });
    };
  }, []);

  // ── Inbound coalescing buffer + rAF flush ───────────────────────────────────
  const pendingRef = useRef<any>(undefined); // accumulated payload between paints
  const pendingOriginRef = useRef<string | undefined>(undefined);
  const rafRef = useRef<number | null>(null);

  const flush = useCallback(() => {
    rafRef.current = null;
    const acc = pendingRef.current;
    pendingRef.current = undefined;
    if (acc === undefined) return;
    if (pendingOriginRef.current) {
      setOriginId(pendingOriginRef.current);
      pendingOriginRef.current = undefined;
    }
    setMessageTick((n) => (n + 1) | 0);
    setWsData(acc as D);
  }, []);

  // Subscribe for the channel's lifetime. Inbound frames are accumulated and
  // flushed on the next animation frame (see header note) rather than each
  // triggering an immediate setState. Keyed on the provider's stable
  // subscribe, not the context value: that changes with connected / busy,
  // and a resubscribe there would be a navigation to WsProvider (a busy tab
  // retries on one). WsProvider sends the subs again on every open.
  const subscribe = ctx?.subscribe;
  useEffect(() => {
    if (!channel || !subscribe) return;
    const onFrame = (msg: WsChannelMessage) => {
      pendingRef.current = coalescePayload(pendingRef.current, msg.p, isFullSet);
      if (msg.origin_id) pendingOriginRef.current = msg.origin_id;
      if (rafRef.current === null) {
        rafRef.current = window.requestAnimationFrame(flush);
      }
    };
    const unsub = subscribe(channel, onFrame);
    return () => {
      unsub();
      if (rafRef.current !== null) {
        window.cancelAnimationFrame(rafRef.current);
        rafRef.current = null;
      }
      pendingRef.current = undefined;
      pendingOriginRef.current = undefined;
    };
  }, [channel, subscribe, flush, isFullSet]);

  // Mirror provider connection state; writes that waited go out on reconnect.
  useEffect(() => {
    setConnected(!!ctx?.connected);
    if (ctx?.connected && outRef.current !== undefined) saveData.current();
  }, [ctx?.connected]);

  const updateData = (
    newData: React.SetStateAction<D | undefined>,
    transmitData: boolean = true,
    clearData: boolean = false
  ) => {
    setWsData(newData);
    setTransmit(transmitData);
    setClear(clearData);
  };

  useEffect(() => {
    if (!transmit) return;
    if (wsData) {
      outRef.current = mergeOutbound(outRef.current, wsData);
      saveData.current();
    }
    if (clear) setWsData(undefined);
    setTransmit(false);
    setClear(false);
  }, [wsData, transmit, clear]);

  // No per-channel hard disconnect — the shared socket is owned by WsProvider.
  // Navigating away unmounts the live tab, which unsubscribes the channel.
  const disconnect = useCallback(() => {}, []);

  return {
    connected,
    originId,
    wsData,
    updateData,
    disconnect,
    messageTick,
    registerReplaceKeys,
  } as const;
};

export default useWsChannel;
