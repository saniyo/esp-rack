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
// falling behind. Array payloads (trend point batches, table row batches) are
// CONCATENATED across coalesced frames so no live data is dropped; scalar fields
// are last-write-wins (the newest snapshot is the truth). rAF also pauses while
// the tab is hidden — no wasted renders in the background.

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
// data; everything else is replaced by the newest value.
const coalescePayload = (acc: any, next: any): any => {
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
    if (Array.isArray(nv) && Array.isArray(av)) {
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

  const doSend = useCallback((data: any) => {
    const ch = chRef.current;
    const c = ctxRef.current;
    if (ch && c) c.send(ch, data);
  }, []);
  const saveData = useRef(throttle(doSend, wsThrottle));

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
  // triggering an immediate setState.
  useEffect(() => {
    if (!channel || !ctx) return;
    const onFrame = (msg: WsChannelMessage) => {
      pendingRef.current = coalescePayload(pendingRef.current, msg.p);
      if (msg.origin_id) pendingOriginRef.current = msg.origin_id;
      if (rafRef.current === null) {
        rafRef.current = window.requestAnimationFrame(flush);
      }
    };
    const unsub = ctx.subscribe(channel, onFrame);
    return () => {
      unsub();
      if (rafRef.current !== null) {
        window.cancelAnimationFrame(rafRef.current);
        rafRef.current = null;
      }
      pendingRef.current = undefined;
      pendingOriginRef.current = undefined;
    };
  }, [channel, ctx, flush]);

  // Mirror provider connection state.
  useEffect(() => {
    setConnected(!!ctx?.connected);
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
    if (wsData) saveData.current(wsData);
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
  } as const;
};

export default useWsChannel;
