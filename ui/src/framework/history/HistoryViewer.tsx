// HistoryViewer — the History page's View tab (docs/plans/history.md): open
// any series, pick columns (whole, single elements, or a row's min/avg/max)
// and a period, and compare it with another period on the same relative
// axis. Each column gets its own chart; all share the x axis.
//
// Deep link: /history/view?series=<id>&cols=<a,b[min]>&range=<24h|7d|30d|1y>
//            (or &from=<t>&to=<t>) [&cmp=<start of the second period>]
import { FC, useEffect, useMemo, useState } from 'react';
import { useSearchParams } from 'react-router-dom';
import {
  Alert, Box, Checkbox, FormControlLabel, MenuItem, TextField, ToggleButton, ToggleButtonGroup, Typography,
} from '@mui/material';
import { CartesianGrid, Legend, Line, LineChart, ResponsiveContainer, Tooltip, XAxis, YAxis } from 'recharts';

import { AXIOS } from '../../api/endpoints';

interface HistCol { key: string; unit?: string; agg: string; width?: number }
interface HistSeries { id: string; title: string; cols: HistCol[]; tiers: { bucket: number; keep: number }[] }
type Cell = number | null | (number | null)[];
interface RowsPage { cols: string[]; rows: Cell[][]; next?: number }

const RANGES: [string, number][] = [['24h', 86400], ['7d', 7 * 86400], ['30d', 30 * 86400], ['1y', 365 * 86400]];
const MAX_PAGES = 20;
const POINTS = 300;
const COLORS = ['#1e88e5', '#e53935', '#43a047', '#fb8c00', '#8e24aa', '#00897b', '#6d4c41', '#3949ab'];

const nowS = () => Math.floor(Date.now() / 1000);
const baseOf = (spec: string) => spec.split('[')[0];

// All pages of rows for one period (following `next`).
export const fetchHistoryRows = async (id: string, cols: string[], from: number, to: number) => {
  const res = Math.max(60, Math.floor((to - from) / POINTS));
  const out: { t: number; v: Record<string, number | null> }[] = [];
  let cursor = from;
  for (let page = 0; page < MAX_PAGES; ++page) {
    const r = await AXIOS.get<RowsPage>(`history/series/${encodeURIComponent(id)}`, {
      params: { from: cursor, to, res, cols: cols.join(',') },
    });
    const names = r.data.cols.slice(1);
    r.data.rows.forEach((row) => {
      const v: Record<string, number | null> = {};
      names.forEach((n, i) => {
        const c = row[i + 1];
        v[n] = Array.isArray(c) ? null : (c as number | null);  // whole arrays aren't charted
      });
      out.push({ t: row[0] as number, v });
    });
    if (r.data.next === undefined || r.data.next <= cursor) break;
    cursor = r.data.next;
  }
  return out;
};

const fmtTime = (t: number, span: number) => {
  const d = new Date(t * 1000);
  return span > 2 * 86400
    ? d.toLocaleDateString([], { day: '2-digit', month: '2-digit' }) + ' ' +
        d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
    : d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
};

const HistoryViewer: FC = () => {
  const [params, setParams] = useSearchParams();
  const [series, setSeries] = useState<HistSeries[]>([]);
  const [error, setError] = useState<string>();
  const [rowsA, setRowsA] = useState<{ t: number; v: Record<string, number | null> }[]>([]);
  const [rowsB, setRowsB] = useState<{ t: number; v: Record<string, number | null> }[]>([]);
  const [loading, setLoading] = useState(false);

  const id = params.get('series') || '';
  const range = params.get('range') || '24h';
  const cols = (params.get('cols') || '').split(',').filter(Boolean);
  const span = RANGES.find(([k]) => k === range)?.[1];
  const to = params.get('to') ? Number(params.get('to')) : nowS();
  const from = params.get('from') ? Number(params.get('from')) : to - (span ?? 86400);
  const cmp = params.get('cmp') ? Number(params.get('cmp')) : undefined;
  const current = series.find((s) => s.id === id);

  const set = (patch: Record<string, string | undefined>) => {
    const next = new URLSearchParams(params);
    Object.entries(patch).forEach(([k, v]) => (v === undefined || v === '' ? next.delete(k) : next.set(k, v)));
    setParams(next, { replace: true });
  };

  useEffect(() => {
    AXIOS.get<{ series: HistSeries[] }>('history/series')
      .then((r) => setSeries(r.data.series || []))
      .catch(() => setError('Could not load the series'));
  }, []);

  const colsKey = cols.join(',');
  useEffect(() => {
    if (!id || !cols.length) {
      setRowsA([]);
      setRowsB([]);
      return;
    }
    let live = true;
    setLoading(true);
    setError(undefined);
    const tasks = [fetchHistoryRows(id, cols, from, to)];
    if (cmp !== undefined) tasks.push(fetchHistoryRows(id, cols, cmp, cmp + (to - from)));
    Promise.all(tasks)
      .then(([a, b]) => {
        if (!live) return;
        setRowsA(a);
        setRowsB(b || []);
      })
      .catch(() => live && setError('Could not load the rows'))
      .finally(() => live && setLoading(false));
    return () => {
      live = false;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [id, colsKey, from, to, cmp]);

  // One chart per column; x = seconds since the start of the period.
  const groups = useMemo(() => {
    const g: Record<string, string[]> = {};
    cols.forEach((c) => (g[baseOf(c)] = [...(g[baseOf(c)] || []), c]));
    return Object.entries(g);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [colsKey]);
  const pointsA = useMemo(() => rowsA.map((r) => ({ x: r.t - from, ...r.v })), [rowsA, from]);
  const pointsB = useMemo(
    () => (cmp === undefined ? [] : rowsB.map((r) => ({ x: r.t - cmp, ...Object.fromEntries(Object.entries(r.v).map(([k, v]) => [`${k} (B)`, v])) }))),
    [rowsB, cmp]
  );

  const toggleCol = (spec: string, on: boolean) => {
    const next = on ? Array.from(new Set([...cols, spec])) : cols.filter((c) => c !== spec);
    set({ cols: next.join(',') });
  };
  const elements = (key: string) =>
    cols.filter((c) => baseOf(c) === key && /\[\d+\]$/.test(c)).map((c) => c.slice(key.length + 1, -1)).join(',');
  const setElements = (key: string, list: string) => {
    const keep = cols.filter((c) => baseOf(c) !== key || !/\[\d+\]$/.test(c));
    const picked = list.split(/[ ,]+/).filter((s) => /^\d+$/.test(s)).map((n) => `${key}[${n}]`);
    set({ cols: [...keep, ...picked].join(',') });
  };

  return (
    <Box>
      {error && <Alert severity="error" sx={{ mb: 2 }}>{error}</Alert>}
      <Box sx={{ display: 'flex', flexWrap: 'wrap', gap: 2, mb: 2, alignItems: 'center' }}>
        <TextField select size="small" label="Series" value={id} sx={{ minWidth: 220 }}
          onChange={(e) => set({ series: e.target.value, cols: undefined })}>
          {series.map((s) => <MenuItem key={s.id} value={s.id}>{s.title || s.id}</MenuItem>)}
        </TextField>
        <ToggleButtonGroup size="small" exclusive value={params.get('from') ? 'custom' : range}
          onChange={(_, v) => v && v !== 'custom' && set({ range: v, from: undefined, to: undefined })}>
          {RANGES.map(([k]) => <ToggleButton key={k} value={k}>{k}</ToggleButton>)}
          <ToggleButton value="custom" disabled>custom</ToggleButton>
        </ToggleButtonGroup>
        <TextField size="small" type="datetime-local" label="From" InputLabelProps={{ shrink: true }}
          value={new Date((from - new Date().getTimezoneOffset() * 60) * 1000).toISOString().slice(0, 16)}
          onChange={(e) => {
            const t = Math.floor(new Date(e.target.value).getTime() / 1000);
            if (t > 0) set({ from: String(t), to: String(t + (to - from)), range: undefined });
          }} />
        <TextField select size="small" label="Compare with" value={cmp === undefined ? '' : cmp === from - (to - from) ? 'prev' : 'custom'}
          sx={{ minWidth: 180 }}
          onChange={(e) => set({ cmp: e.target.value === 'prev' ? String(from - (to - from)) : undefined })}>
          <MenuItem value="">—</MenuItem>
          <MenuItem value="prev">the period before</MenuItem>
          {cmp !== undefined && cmp !== from - (to - from) && <MenuItem value="custom">from the link</MenuItem>}
        </TextField>
      </Box>

      {current && (
        <Box sx={{ display: 'flex', flexWrap: 'wrap', gap: 2, mb: 2 }}>
          {current.cols.map((c) => (c.width ?? 1) === 1 ? (
            <FormControlLabel key={c.key} label={`${c.key}${c.unit ? `, ${c.unit}` : ''}`}
              control={<Checkbox checked={cols.includes(c.key)} onChange={(e) => toggleCol(c.key, e.target.checked)} />} />
          ) : (
            <Box key={c.key} sx={{ display: 'flex', alignItems: 'center', gap: 1 }}>
              <Typography variant="body2">{c.key}{c.unit ? `, ${c.unit}` : ''} ({c.width}):</Typography>
              {['min', 'avg', 'max'].map((d) => (
                <FormControlLabel key={d} label={d} control={
                  <Checkbox size="small" checked={cols.includes(`${c.key}[${d}]`)}
                    onChange={(e) => toggleCol(`${c.key}[${d}]`, e.target.checked)} />} />
              ))}
              <TextField size="small" label="elements" placeholder="5, 17" sx={{ width: 120 }}
                defaultValue={elements(c.key)} onBlur={(e) => setElements(c.key, e.target.value)} />
            </Box>
          ))}
        </Box>
      )}

      {loading && <Typography variant="body2" color="text.secondary">Loading…</Typography>}
      {!loading && id && cols.length > 0 && !rowsA.length && (
        <Typography variant="body2" color="text.secondary">No records in this period.</Typography>
      )}
      {groups.map(([key, specs]) => (
        <Box key={key} sx={{ height: 220, mb: 2 }}>
          <Typography variant="subtitle2">{key}</Typography>
          <ResponsiveContainer width="100%" height="90%">
            <LineChart>
              <CartesianGrid strokeDasharray="3 3" />
              <XAxis dataKey="x" type="number" domain={[0, to - from]} allowDuplicatedCategory={false}
                tickFormatter={(x: number) => fmtTime(from + x, to - from)} />
              <YAxis domain={['auto', 'auto']} width={56} />
              <Tooltip labelFormatter={(x: any) => fmtTime(from + Number(x), to - from)} />
              <Legend />
              {specs.map((s, i) => (
                <Line key={s} data={pointsA} dataKey={s} name={s} dot={false} connectNulls={false}
                  stroke={COLORS[i % COLORS.length]} isAnimationActive={false} />
              ))}
              {cmp !== undefined && specs.map((s, i) => (
                <Line key={`${s}-b`} data={pointsB} dataKey={`${s} (B)`} name={`${s} (compare)`} dot={false}
                  strokeDasharray="4 3" stroke={COLORS[i % COLORS.length]} isAnimationActive={false} />
              ))}
            </LineChart>
          </ResponsiveContainer>
        </Box>
      ))}
    </Box>
  );
};

export default HistoryViewer;
