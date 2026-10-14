// SES-LIB-029: form layout from the backend. Layout markers in the field
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
  'text', 'textarea', 'secret', 'table', 'trend', 'message', 'action', 'files', 'upload', 'progress', 'radio',
]);
const FULL_TYPES_RO = new Set(['textarea', 'table', 'trend', 'message', 'action', 'files', 'upload']);

export const spanOf = (o?: string): number | undefined => {
  const m = /(?:^|;)sp=(\d+)/.exec(o ?? '');
  return m ? Number(m[1]) : undefined;
};
export const fullOf = (o?: string): boolean => /(?:^|;)fr(?:=1)?(?:;|$)/.test(o ?? '');

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

export type Responsive = string | { xs: string; sm: string; md: string };

// grid-template-columns of a segment: one column unless the page asks for
// more with Cols(n) (2 on small, 1 on phone screens). Grouping is the page
// author's call, never automatic (SES-LIB-029c).
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

// A read-only value as a compact tile (Status tabs): a small icon that keeps
// its status colour, the label as a caption, the value as the main text, no
// dividers. Applied to the grid cell, so every field component is covered.
export const READ_ONLY_TILE_SX = {
  '& .MuiListItem-root': { px: 1, py: 0.25 },
  '& .MuiDivider-root': { display: 'none' },
  '& .MuiListItemText-root': { my: 0 },
  '& .MuiListItemText-primary': { fontSize: '0.75rem', color: 'text.secondary', lineHeight: 1.3 },
  '& .MuiListItemText-secondary': { fontSize: '0.875rem', color: 'text.primary', overflowWrap: 'anywhere' },
};

// Every grid cell, editable or not: the same small icon, long values wrap
// inside their cell (SES-LIB-029c).
export const CELL_SX = {
  '& .MuiListItemAvatar-root': { minWidth: 30 },
  '& .MuiAvatar-root': { width: 22, height: 22 },
  '& .MuiAvatar-root svg': { fontSize: 14 },
  '& .MuiListItemText-root, & .MuiListItemText-secondary': { overflowWrap: 'anywhere' },
};

// A heading worth drawing: not empty and not already shown above (the
// feature, menu or tab title), ignoring case and spaces. '' = don't draw.
export const visibleHeading = (text: string | undefined, seen: (string | undefined)[]): string => {
  const norm = (s?: string) => (s ?? '').replace(/\s+/g, ' ').trim().toLowerCase();
  const t = norm(text);
  if (!t) return '';
  return seen.some((x) => norm(x) === t) ? '' : (text ?? '').trim();
};
