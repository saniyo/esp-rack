// form layout from the backend. Layout markers in the field
// stream (web::Cols(n) / web::Break(), parsed into the pseudo types below)
// cut a form into segments; each segment is one CSS grid. A field's own
// "sp=<k>" / "fr" options span columns or the whole row; without them the
// type decides. One rule for every form.

export const COLS_TYPE = '__cols';
export const BRK_TYPE = '__brk';

export const isLayoutMarker = (f: { type?: string }): boolean => f.type === COLS_TYPE || f.type === BRK_TYPE;

// The wire marker objects ({"t":"cols","n":2} / {"t":"brk"}) as fields.
export const markerFromRaw = (raw: any, idx: number) => {
  if (!raw || typeof raw !== 'object' || 'o' in raw) return undefined;
  if (raw.t === 'cols') return { label: `__cols${idx}`, type: COLS_TYPE, value: Number(raw.n) || 1, o: '' };
  if (raw.t === 'brk') return { label: `__brk${idx}`, type: BRK_TYPE, value: 0, o: '' };
  return undefined;
};

// Types that take the whole row unless told otherwise. Read-only (Status
// tabs): only what can't be a tile; a read-only text is a tile.
const FULL_TYPES = new Set([
  'text', 'textarea', 'secret', 'table', 'trend', 'message', 'action', 'files', 'upload', 'progress', 'radio', 'flow',
]);
const FULL_TYPES_RO = new Set(['textarea', 'table', 'trend', 'message', 'action', 'files', 'upload', 'flow']);

export const spanOf = (o?: string): number | undefined => {
  const m = /(?:^|;)sp=(\d+)/.exec(o ?? '');
  return m ? Number(m[1]) : undefined;
};
export const fullOf = (o?: string): boolean => /(?:^|;)fr(?:=1)?(?:;|$)/.test(o ?? '');
// tile=1 (text / number): shown as an overview tile, laid out as read-only.
export const tileOf = (o?: string): boolean => /(?:^|;)tile(?:=1)?(?:;|$)/.test(o ?? '');

export interface Segment<T> {
  cols?: number;  // undefined = flow
  cells: T[];
}

// Splits entries at the markers; empty segments are dropped.
export function segmentFields<T>(entries: T[], fieldOf: (e: T) => { type?: string; value?: any }): Segment<T>[] {
  const out: Segment<T>[] = [];
  let cur: Segment<T> = { cells: [] };
  for (const e of entries) {
    const f = fieldOf(e);
    if (f.type === COLS_TYPE || f.type === BRK_TYPE) {
      if (cur.cells.length) out.push(cur);
      cur = { cols: f.type === COLS_TYPE ? Math.max(1, Number(f.value) || 1) : undefined, cells: [] };
      continue;
    }
    cur.cells.push(e);
  }
  if (cur.cells.length) out.push(cur);
  return out;
}

export interface FieldGroup<T> {
  card?: string;  // undefined = the bare fields between cards
  entries: T[];
}

// Cuts a form into cards (consecutive fields with one card=<id>) and the
// bare runs between them. A layout marker goes with the field after it:
// Cols(n) / Break() before a card's first field lays out that card, not an
// empty tail of the group before it. Markers at the very end stay with the
// last group (segmentFields drops them).
export function groupByCard<T>(
  entries: T[],
  fieldOf: (e: T) => { type?: string },
  cardOf: (e: T) => string | undefined
): FieldGroup<T>[] {
  const out: FieldGroup<T>[] = [];
  let markers: T[] = [];
  for (const e of entries) {
    if (isLayoutMarker(fieldOf(e))) {
      markers.push(e);
      continue;
    }
    const card = cardOf(e);
    const last = out[out.length - 1];
    if (last && last.card === card) last.entries.push(...markers, e);
    else out.push({ card, entries: [...markers, e] });
    markers = [];
  }
  if (markers.length && out.length) out[out.length - 1].entries.push(...markers);
  return out;
}

export type Responsive = string | { xs: string; sm: string; md: string };

// grid-template-columns of a segment: one column unless the page asks for
// more with Cols(n) (2 on small, 1 on phone screens). Grouping is the page
// author's call, never automatic.
export function gridTemplateFor(cols?: number): Responsive {
  if (!cols) return '1fr';
  return { xs: '1fr', sm: `repeat(${Math.min(cols, 2)}, 1fr)`, md: `repeat(${cols}, 1fr)` };
}

// grid-column of a field in a segment with `cols` columns (flow: up to 2).
export function gridColumnFor(
  type: string | undefined,
  span?: number,
  full?: boolean,
  cols?: number,
  readOnly?: boolean,
): Responsive {
  if (full || (!span && (readOnly ? FULL_TYPES_RO : FULL_TYPES).has(type ?? ''))) return '1 / -1';
  if (!span || span <= 1) return 'auto';
  const n = cols ?? 2;
  return { xs: '1 / -1', sm: `span ${Math.min(span, n, 2)}`, md: `span ${Math.min(span, n)}` };
}

// A number field's value as the input wants it: trimmed text, '' for none.
export const numberInputValue = (v: unknown): string | number => {
  if (v === undefined || v === null) return '';
  return typeof v === 'string' ? v.trim() : (v as number);
};

// The arrow step of a number input: the field's st= option, else the
// format's decimals (f=0.0 -> 0.1, f=0.00 -> 0.01, f=0 -> 1), else none.
export const numberStep = (o: string | undefined): number | undefined => {
  const opts = (o || '').split(';');
  const st = opts.find((s) => s.startsWith('st='));
  if (st) {
    const n = parseFloat(st.slice(3));
    if (Number.isFinite(n) && n > 0) return n;
  }
  const f = opts.find((s) => s.startsWith('f='));
  if (!f) return undefined;
  const dot = f.indexOf('.');
  const decimals = dot < 0 ? 0 : f.length - dot - 1;
  return decimals > 0 ? Number((10 ** -decimals).toFixed(decimals)) : 1;
};

// Decimals of a "0.0"-style format (the raw `f=` option); 0 without one.
export const decimalsOf = (o: string | undefined): number => {
  const m = /(?:^|;)f=([^;]*)/.exec(o ?? '');
  const dot = m ? m[1].indexOf('.') : -1;
  return dot >= 0 ? m![1].length - dot - 1 : 0;
};

// Every grid cell: elements keep their own sizes; long values
// wrap inside their cell instead of pushing neighbours; the grid separates
// rows, so no dividers.
export const CELL_SX = {
  '& .MuiListItemText-root, & .MuiListItemText-secondary': { overflowWrap: 'anywhere' },
  '& .MuiDivider-root': { display: 'none' },
};

// A heading worth drawing: not empty and not already shown above (the
// feature, menu or tab title), ignoring case and spaces. '' = don't draw.
export const visibleHeading = (text: string | undefined, seen: (string | undefined)[]): string => {
  const norm = (s?: string) => (s ?? '').replace(/\s+/g, ' ').trim().toLowerCase();
  const t = norm(text);
  if (!t) return '';
  return seen.some((x) => norm(x) === t) ? '' : (text ?? '').trim();
};
