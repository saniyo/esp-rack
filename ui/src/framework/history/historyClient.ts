// historyClient: the History module's REST for the charts' history mode.
//   * listSeries(): the series, plus the device's clock (periods are planned
//     on it, never on the browser's) and how many values one page holds.
//   * fetchRows(): rows of one tier, following `next` (the original from and
//     tier are always resent), columns packed so a page fits the device.
//   * One request at a time per page: the device reads on async_tcp and
//     blocks it meanwhile.
import { AXIOS } from '../../api/endpoints';
import { fileNameOf, saveBlob } from '../../utils/download';

export interface HistCol {
  key: string;
  unit?: string;
  agg: string;  // sum | avg | min | max | last
  width?: number;
}
export interface HistTierInfo {
  bucket: number;  // s; 0 = a put-series
  keep: number;    // s
}
export interface HistSeriesInfo {
  id: string;
  title: string;
  cols: HistCol[];
  tiers: HistTierInfo[];
  oldest?: number;
  newest?: number;
  local_days?: boolean;
  full?: number;
  stale?: number;
}
export interface HistList {
  series: HistSeriesInfo[];
  maxv: number;     // values one page of rows can hold
  offsetS: number;  // device clock - browser clock
}

export type HistErrorKind = 'missing' | 'tooWide' | 'badCols' | 'busy' | 'other';
export class HistError extends Error {
  constructor(public kind: HistErrorKind, message: string) {
    super(message);
  }
}

const MAX_PAGES = 20;

let tail: Promise<unknown> = Promise.resolve();
const enqueue = <T,>(fn: () => Promise<T>): Promise<T> => {
  const run = tail.then(fn, fn);
  tail = run.catch(() => undefined);
  return run;
};

// One retry after 1 s when the device is short of memory (503).
const get = async (url: string, params: Record<string, unknown>, signal?: AbortSignal): Promise<any> => {
  for (let attempt = 0; ; ++attempt) {
    try {
      // eslint-disable-next-line no-await-in-loop
      const r = await enqueue(() => AXIOS.get(url, { params, signal }));
      return r.data;
    } catch (e: any) {
      const status = e?.response?.status;
      if (status === 503 && attempt === 0 && !signal?.aborted) {
        // eslint-disable-next-line no-await-in-loop
        await new Promise((ok) => setTimeout(ok, 1000));
        continue;
      }
      if (status === 400) throw new HistError('badCols', "This chart's history no longer matches the series");
      if (status === 404) throw new HistError('missing', 'No such history series');
      if (status === 503) throw new HistError('busy', 'Device busy');
      throw e;
    }
  }
};

let listPromise: Promise<HistList> | null = null;

export function listSeries(refresh = false): Promise<HistList> {
  if (!listPromise || refresh) {
    const asked = Date.now();
    listPromise = get('history/series', {}).then((d) => {
      const mid = (asked + Date.now()) / 2000;
      return {
        series: Array.isArray(d?.series) ? (d.series as HistSeriesInfo[]) : [],
        maxv: typeof d?.maxv === 'number' && d.maxv > 0 ? d.maxv : 200,
        offsetS: typeof d?.now === 'number' ? d.now - mid : 0,
      };
    });
    listPromise.catch(() => {
      listPromise = null;
    });
  }
  return listPromise;
}

export const deviceNow = (l: HistList): number => Math.floor(Date.now() / 1000 + l.offsetS);

export interface HistQuery {
  id: string;
  cols: string[];    // server syntax: key, key[5], key[min]
  widths: number[];  // values per col (a whole width-N column: N)
  from: number;
  to: number;
  tier: number;
  res?: number;      // undefined: the tier's own records; 0: one row
}
export interface HistRows {
  cols: string[];   // 't' first
  rows: any[][];
  truncated: boolean;
}

// Columns in groups of at most maxv values; one group per request.
const pack = (q: HistQuery, maxv: number): number[][] => {
  const groups: number[][] = [];
  let cur: number[] = [];
  let used = 0;
  q.cols.forEach((_, i) => {
    const w = Math.max(1, q.widths[i] || 1);
    if (w > maxv) throw new HistError('tooWide', "This chart's history is too wide for this device's memory");
    if (used + w > maxv && cur.length) {
      groups.push(cur);
      cur = [];
      used = 0;
    }
    cur.push(i);
    used += w;
  });
  if (cur.length) groups.push(cur);
  return groups;
};

export async function fetchRows(q: HistQuery, maxv: number, signal?: AbortSignal): Promise<HistRows> {
  const url = `history/series/${encodeURIComponent(q.id)}`;
  const byT = new Map<number, any[]>();
  let truncated = false;
  for (const group of pack(q, maxv)) {
    const params: Record<string, unknown> = {
      from: q.from,
      to: q.to,
      tier: q.tier,
      cols: group.map((i) => q.cols[i]).join(','),
    };
    if (q.res !== undefined) params.res = q.res;
    let cursor: number | undefined;
    for (let page = 0; ; ++page) {
      if (page === MAX_PAGES) {
        truncated = true;
        break;
      }
      // eslint-disable-next-line no-await-in-loop
      const d = await get(url, cursor === undefined ? params : { ...params, cursor }, signal);
      (Array.isArray(d?.rows) ? d.rows : []).forEach((row: any[]) => {
        const t = row[0] as number;
        const out = byT.get(t) || [t, ...q.cols.map(() => null)];
        group.forEach((ci, k) => {
          out[ci + 1] = row[k + 1] ?? null;
        });
        byT.set(t, out);
      });
      if (typeof d?.next !== 'number' || (cursor !== undefined && d.next <= cursor)) break;
      cursor = d.next;
    }
  }
  return {
    cols: ['t', ...q.cols],
    rows: Array.from(byT.values()).sort((a, b) => a[0] - b[0]),
    truncated,
  };
}

// The same rows as a CSV file, saved by the browser.
export async function exportCsv(q: HistQuery): Promise<void> {
  const params: Record<string, unknown> = { format: 'csv', from: q.from, to: q.to, tier: q.tier, cols: q.cols.join(',') };
  if (q.res !== undefined) params.res = q.res;
  const r = await enqueue(() =>
    AXIOS.get(`history/series/${encodeURIComponent(q.id)}`, { params, responseType: 'blob' })
  );
  saveBlob(r.data as Blob, fileNameOf((r.headers as any)?.['content-disposition']) || `${q.id}.csv`);
}
