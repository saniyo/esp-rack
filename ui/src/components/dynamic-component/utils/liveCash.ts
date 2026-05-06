// src/utils/liveCash.ts
type Label = string;
export type Source = 'ws' | 'rest';

type Listener = (label: Label, value: any, source: Source, ts: number) => void;

interface Entry {
  value: any;
  ts: number;
  source: Source;
}

const store = new Map<Label, Entry>();
const listeners = new Set<Listener>();

// Age-based pruning. Map entries are otherwise never released — navigating
// across features (each with its own label set) would accumulate stale
// labels for the whole tab lifetime. We keep the last 10 minutes of writes
// and sweep opportunistically on every write, throttled to once per minute
// so the sweep cost stays negligible even under 10 Hz LightState traffic.
const LIVE_CACHE_TTL_MS = 10 * 60 * 1000;
const LIVE_CACHE_SWEEP_INTERVAL_MS = 60 * 1000;
let lastSweepAt = 0;

function sweepStaleEntries(now: number): void {
  if (now - lastSweepAt < LIVE_CACHE_SWEEP_INTERVAL_MS) return;
  lastSweepAt = now;
  const cutoff = now - LIVE_CACHE_TTL_MS;
  store.forEach((entry, label) => {
    if (entry.ts < cutoff) store.delete(label);
  });
}

// повертає голе значення (як і раніше)
export function getLiveValue(label: Label): any | undefined {
  return store.has(label) ? store.get(label)!.value : undefined;
}

export function getLiveSnapshot(labels: Label[]): Record<string, any> {
  const out: Record<string, any> = {};
  labels.forEach((l) => {
    if (store.has(l)) out[l] = store.get(l)!.value;
  });
  return out;
}

// ОНОВЛЕННЯ: пишемо з джерелом і timestamp
export function setLiveValue(label: Label, value: any, source: Source = 'ws'): void {
  const now = Date.now();
  sweepStaleEntries(now);
  const prev = store.get(label);
  if (!prev || now >= prev.ts) {
    store.set(label, { value, ts: now, source });
    listeners.forEach((l) => l(label, value, source, now));
  }
}

// bulk-оновлення (зручно для SAVE)
export function setLiveValuesBulk(patch: Record<string, any>, source: Source = 'rest'): void {
  const now = Date.now();
  sweepStaleEntries(now);
  Object.keys(patch).forEach((label, i) => {
    const ts = now + i; // гарантуємо зростаючий ts у межах однієї операції
    const prev = store.get(label);
    if (!prev || ts >= prev.ts) {
      const value = patch[label];
      store.set(label, { value, ts, source });
      listeners.forEach((l) => l(label, value, source, ts));
    }
  });
}

export function subscribeLiveValues(fn: Listener): () => void {
  listeners.add(fn);
  return () => { listeners.delete(fn); };
}
