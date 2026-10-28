// useHistoryData: a Trend's history mode (docs/plans/history.md). Owns the
// period choice (kept per browser), plans the window on the device's clock,
// reads the series and turns the answer into the chart's own rows.
// Category charts only for now: one row over the period (res=0), a width-N
// column transposed to N category rows (averages, or totals for Sum).
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import {
  deviceNow, exportCsv, fetchRows, HistError, HistList, HistQuery, HistSeriesInfo, listSeries,
} from '../../../../framework/history/historyClient';
import { ResolvedZone } from '../../../../utils/deviceTime';
import { wallParts, wallToEpoch } from '../../utils/dateTimeParts';
import { ALL_PERIODS, bucketWord, HistPeriod, Plan, planSeries, planWindow, timeTicks } from './historyPlan';

export interface HistLine {
  dataKey: string;
  spec?: string;  // the history column (server syntax); default dataKey
  sign: number;
}

export interface HistoryBinding {
  series: string;
  periods: HistPeriod[];
  initial: HistPeriod;
  liveLabel: string;
}

export interface HistoryState {
  available: boolean;   // the series exists: show the switch
  period: HistPeriod;
  setPeriod: (p: HistPeriod) => void;
  day?: number;
  setDay: (d: number) => void;
  minDay?: number;
  maxDay: number;
  active: boolean;      // a history period (not Live)
  loading: boolean;
  rows: Record<string, any>[] | null;
  status: string;
  notes: string[];
  empty?: { title: string; detail?: string };
  error?: string;
  unit?: string;
  timeSeries: boolean;            // x = epoch s on a number axis
  xDomain?: [number, number];
  ticks?: number[];
  reload: () => void;
  csv?: () => void;
  zone: ResolvedZone;
}

const pad = (n: number, w = 2) => String(n).padStart(w, '0');
const WEEKDAY = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

const store = (key: string) => {
  try {
    const v = window.localStorage.getItem(key);
    return v ? JSON.parse(v) : undefined;
  } catch {
    return undefined;
  }
};
const save = (key: string, v: unknown) => {
  try {
    window.localStorage.setItem(key, JSON.stringify(v));
  } catch {
    // private mode / full storage: the choice is just not remembered
  }
};

export function useHistoryData(opts: {
  binding?: HistoryBinding;
  lines: HistLine[];
  xAxisKey: string;
  isCategory: boolean;
  liveRows: Record<string, any>[];
  zone: ResolvedZone;
  storageKey: string;
}): HistoryState {
  const { binding, lines, xAxisKey, isCategory, liveRows, zone, storageKey } = opts;

  const remembered = useMemo(() => store(storageKey) || {}, [storageKey]);
  const [period, setPeriodState] = useState<HistPeriod>(() => {
    const p = remembered.period as HistPeriod | undefined;
    return p && binding?.periods.includes(p) ? p : binding?.initial ?? 'live';
  });
  const [day, setDayState] = useState<number | undefined>(remembered.day);
  const [reloadTick, setReloadTick] = useState(0);
  const [list, setList] = useState<HistList | null>(null);
  const [info, setInfo] = useState<HistSeriesInfo | null>(null);
  const [rows, setRows] = useState<Record<string, any>[] | null>(null);
  const [plan, setPlan] = useState<Plan | null>(null);
  const [res, setRes] = useState(0);
  const [gap, setGap] = useState<[number, number] | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string>();
  const queryRef = useRef<HistQuery | null>(null);

  // Local days on the device's zone.
  const dayStart = useCallback(
    (t: number) => {
      const w = wallParts(t, zone);
      return wallToEpoch({ ...w, h: 0, mi: 0, s: 0 }, zone);
    },
    [zone]
  );

  const setPeriod = (p: HistPeriod) => {
    setPeriodState(p);
    save(storageKey, { period: p, day });
  };
  const setDay = (d: number) => {
    setDayState(d);
    save(storageKey, { period, day: d });
  };
  const reload = () => setReloadTick((n) => n + 1);

  // The series list (cached per page load; Reload refreshes it).
  useEffect(() => {
    if (!binding) return;
    let live = true;
    listSeries(reloadTick > 0)
      .then((l) => {
        if (!live) return;
        setList(l);
        setInfo(l.series.find((s) => s.id === binding.series) || null);
      })
      .catch(() => live && setInfo(null));
    return () => {
      live = false;
    };
  }, [binding, reloadTick]);

  const active = !!binding && !!info && period !== 'live';
  const now = list ? deviceNow(list) : Math.floor(Date.now() / 1000);
  const today = dayStart(now);
  const pickedDay = day !== undefined && day <= today ? day : today;

  // Columns: each declared line's history column, its width and agg.
  const columns = useMemo(() => {
    if (!info) return [];
    return lines.map((l) => {
      const spec = l.spec || l.dataKey;
      const base = spec.split('[')[0];
      const col = info.cols.find((c) => c.key === base);
      const whole = !spec.includes('[');
      return { line: l, spec, col, width: whole ? col?.width ?? 1 : 1 };
    }).filter((c) => !!c.col);  // a line the series has no column for is live only
  }, [info, lines]);
  const unmatched = !!info && lines.length > 0 && columns.length === 0;
  const isSum = columns.some((c) => c.col?.agg === 'sum');
  const unit = columns.find((c) => c.col?.unit)?.col?.unit;

  useEffect(() => {
    if (!active || !info || !list) {
      setRows(null);
      setPlan(null);
      return;
    }
    const input = {
      period: period as Exclude<HistPeriod, 'live'>,
      day: pickedDay,
      now: deviceNow(list),
      oldest: info.oldest || 0,
      tiers: info.tiers,
      localDays: !!info.local_days,
      dayStart,
    };
    // A category chart reads one row over the period; a time series reads
    // rows at a resolution the period needs.
    const planned = isCategory ? { plan: planWindow(input), res: 0 } : planSeries(input);
    const pl = planned.plan;
    setPlan(pl);
    setRes(planned.res);
    setGap(null);
    setError(undefined);
    const wholeOverTime = !isCategory && columns.some((c) => c.width > 1);
    if (pl.empty || unmatched || wholeOverTime) {
      if (unmatched) setError("This chart's history no longer matches the series");
      else if (wholeOverTime) setError('Over time a line needs one value: name an element or [avg]');
      setRows(null);
      queryRef.current = null;
      return;
    }
    const q: HistQuery = {
      id: info.id,
      cols: columns.map((c) => c.spec),
      widths: columns.map((c) => c.width),
      from: pl.fromEff,
      to: pl.to - 1,
      tier: pl.tier,
      res: planned.res,
    };
    queryRef.current = q;
    const ctl = new AbortController();
    setLoading(true);
    fetchRows(q, list.maxv, ctl.signal)
      .then((r) => {
        if (ctl.signal.aborted) return;
        if (!isCategory) {
          // Time series: x = epoch s; a null row where records are missing
          // (more than 1.5 steps apart), so the lines break there.
          const out: Record<string, any>[] = [];
          let prev: number | undefined;
          let firstGap: [number, number] | null = null;
          r.rows.forEach((row) => {
            const t = row[0] as number;
            if (prev !== undefined && t - prev > 1.5 * planned.res) {
              const g: Record<string, any> = { [xAxisKey]: prev + planned.res };
              columns.forEach((c) => (g[c.line.dataKey] = null));
              out.push(g);
              if (!firstGap) firstGap = [prev + planned.res, t];
            }
            const o: Record<string, any> = { [xAxisKey]: t };
            columns.forEach((c, ci) => {
              const v = row[ci + 1];
              o[c.line.dataKey] = typeof v === 'number' ? v * c.line.sign : null;
            });
            out.push(o);
            prev = t;
          });
          setGap(firstGap);
          setRows(out);
          return;
        }
        const row = r.rows[0];
        if (!row) {
          setRows([]);
          return;
        }
        // Transpose: element i of every column -> category row i.
        const values = columns.map((_, i) => row[i + 1]);
        const n = Math.max(1, ...values.map((v) => (Array.isArray(v) ? v.length : 1)));
        const live = liveRows.map((lr) => lr[xAxisKey]);
        const labelOf = (i: number) =>
          live.length === n && live[i] !== undefined ? live[i] : pad(i + 1, n >= 100 ? 3 : 2);
        const out: Record<string, any>[] = [];
        for (let i = 0; i < n; ++i) {
          const o: Record<string, any> = { [xAxisKey]: labelOf(i) };
          columns.forEach((c, ci) => {
            const v = Array.isArray(values[ci]) ? values[ci][i] : values[ci];
            o[c.line.dataKey] = typeof v === 'number' ? v * c.line.sign : null;
          });
          out.push(o);
        }
        setRows(out);
      })
      .catch((e) => {
        if (ctl.signal.aborted) return;
        setRows(null);
        setError(e instanceof HistError ? (e.kind === 'busy' ? 'Device busy — press Reload' : e.message) : 'Could not read the history');
      })
      .finally(() => !ctl.signal.aborted && setLoading(false));
    return () => ctl.abort();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [active, info, list, period, pickedDay, columns, unmatched, dayStart, isCategory]);

  // ── words ──
  const dm = (t: number) => {
    const w = wallParts(t, zone);
    return `${pad(w.d)}.${pad(w.mo)}`;
  };
  const dmy = (t: number) => {
    const w = wallParts(t, zone);
    return `${pad(w.d)}.${pad(w.mo)}.${w.y}`;
  };
  const hm = (t: number) => {
    const w = wallParts(t, zone);
    return `${pad(w.h)}:${pad(w.mi)}`;
  };
  const wd = (t: number) => {
    const w = wallParts(t, zone);
    return WEEKDAY[new Date(Date.UTC(w.y, w.mo - 1, w.d)).getUTCDay()];
  };
  const at = (t: number) => (t === today ? 'today 00:00' : dayStart(t) === today ? hm(t) : `${dm(t)} ${hm(t)}`);

  let status = '';
  const notes: string[] = [];
  let empty: HistoryState['empty'];
  if (active && plan) {
    const what = !isCategory
      ? `${bucketWord(res || plan.bucket)} ${isSum ? 'totals' : 'averages'}`
      : isSum
        ? `totals from ${bucketWord(plan.bucket)} records`
        : `averages of ${bucketWord(plan.bucket)} records`;
    const spanName: Record<string, string> = { '24h': 'Last 24 h', '7d': 'Last 7 days', '30d': 'Last 30 days', '1y': 'Last year' };
    if (period === 'date') {
      const d = plan.from;
      status = `${wd(d)} ${dm(d)} ${plan.openEnd ? `to ${hm(plan.to)}` : '· whole day'} · ${what}`;
    } else if (plan.clamped) {
      status = `${dmy(plan.fromEff)} ${hm(plan.fromEff)} – ${at(plan.to)} · ${what}`;
    } else {
      status = `${spanName[period]} to ${at(plan.to)} · ${what}`;
    }
    if (plan.clamped && period !== 'date') {
      notes.push(`History starts ${dmy(plan.fromEff)} ${hm(plan.fromEff)}: the period begins there.`);
    }
    if (isSum && plan.openEnd && !plan.empty) {
      notes.push(
        plan.bucket >= 86400
          ? `Today is still open: totals to ${at(plan.to)}.`
          : `The current ${plan.bucket === 3600 ? 'hour' : 'bucket'} is still open: totals to ${at(plan.to)}.`
      );
    }
    if (isCategory && !isSum && plan.bucket >= 86400) notes.push('Daily averages smooth out short peaks; a shorter period shows them.');
    if (gap) notes.push(`No records ${dm(gap[0])} ${hm(gap[0])}–${dm(gap[1]) === dm(gap[0]) ? '' : `${dm(gap[1])} `}${hm(gap[1])}: the lines break there.`);
    const unitName = plan.bucket === 3600 ? 'hour' : plan.bucket >= 86400 ? 'day' : `${bucketWord(plan.bucket)} bucket`;
    const firstClose = plan.to + plan.bucket;
    if (plan.empty === 'none') {
      empty = {
        title: 'No records yet',
        detail: `Records are stored per ${unitName}; the first one closes at ${hm(firstClose)}.${binding?.liveLabel && binding.liveLabel !== 'Live' ? ` "${binding.liveLabel}" shows the running values now.` : ''}`,
      };
    } else if (plan.empty === 'before') {
      empty = {
        title: period === 'date' ? `No records for ${wd(plan.from)} ${dm(plan.from)}` : 'No history for this period yet',
        detail: `Recording started ${dmy(info!.oldest!)} ${hm(info!.oldest!)}.`,
      };
    } else if (plan.empty === 'noClosed') {
      empty = { title: `No closed ${unitName} yet`, detail: `The first one closes at ${hm(firstClose)}.` };
    } else if (rows && rows.length === 0) {
      empty = { title: 'No records in this period' };
    }
  }

  const csv =
    active && plan && !plan.empty && queryRef.current
      ? () => {
          if (queryRef.current) exportCsv(queryRef.current).catch(() => setError('Could not export the CSV'));
        }
      : undefined;

  return {
    available: !!binding && !!info,
    period: info ? period : 'live',
    setPeriod,
    day: pickedDay,
    setDay,
    minDay: info?.oldest ? dayStart(info.oldest) : undefined,
    maxDay: today,
    active,
    loading,
    rows: active ? rows : null,
    status,
    notes,
    empty,
    error,
    unit,
    timeSeries: !isCategory,
    xDomain: active && plan && !isCategory ? [plan.fromEff, plan.to] : undefined,
    ticks: active && plan && !isCategory ? timeTicks(plan.fromEff, plan.to, zoneOffset(plan.to, zone)) : undefined,
    reload,
    csv,
    zone,
  };
}

// Seconds east of UTC on the device's wall clock at t.
const zoneOffset = (t: number, zone: ResolvedZone): number => {
  const w = wallParts(t, zone);
  return Date.UTC(w.y, w.mo - 1, w.d, w.h, w.mi, w.s) / 1000 - t;
};

export const parsePeriods = (raw: unknown): HistPeriod[] => {
  const list = Array.isArray(raw) ? (raw as string[]) : [];
  const ok = list.filter((p): p is HistPeriod => (ALL_PERIODS as string[]).includes(p));
  return ok.length ? ok : ALL_PERIODS;
};
