// Device-local time formatting (SES-LIB-015).
//
// The device's timezone comes from the manifest (`device.tz = {iana, posix}`,
// see WebManager::addDeviceTz). Times are shown in THAT zone, not the
// browser's: an operator abroad still sees the schedule the device runs.
//
//   * IANA label known  -> Intl.DateTimeFormat with timeZone: iana (DST-aware)
//   * POSIX string only -> its standard offset as a fixed zone, hint "UTC±N"
//   * nothing           -> the browser's zone (the previous behaviour)
//
// Pure functions (no React), so they're unit-tested in jest.

export interface DeviceTz {
  iana?: string;
  posix?: string;
}

export interface ResolvedZone {
  timeZone?: string;   // IANA name for Intl; undefined = browser zone
  offsetSec?: number;  // fixed offset east of UTC (POSIX fallback)
  hint?: string;       // "UTC+2" when only a fixed offset is known
}

export type XFormat = 'epoch' | 'time' | 'date' | 'datetime' | 'dom' | 'month';

const XFORMATS: readonly string[] = ['epoch', 'time', 'date', 'datetime', 'dom', 'month'];

export function isXFormat(s: unknown): s is XFormat {
  return typeof s === 'string' && XFORMATS.includes(s);
}

function validIana(name: string): boolean {
  try {
    new Intl.DateTimeFormat('en-US', { timeZone: name });
    return true;
  } catch {
    return false;
  }
}

// POSIX TZ standard offset, east of UTC in seconds. "EET-2EEST,M3.5.0/3,…"
// -> +7200 (POSIX signs are inverted). "<+0530>-5:30" -> +19800.
export function posixStdOffsetSec(posix: string): number | undefined {
  const m = /^(?:<[^>]+>|[A-Za-z]{3,})([+-]?)(\d{1,2})(?::(\d{2}))?(?::(\d{2}))?/.exec(posix.trim());
  if (!m) return undefined;
  const sign = m[1] === '-' ? 1 : -1;
  const sec = Number(m[2]) * 3600 + Number(m[3] || 0) * 60 + Number(m[4] || 0);
  return sign * sec;
}

function offsetHint(offsetSec: number): string {
  const sign = offsetSec < 0 ? '-' : '+';
  const abs = Math.abs(offsetSec);
  const h = Math.floor(abs / 3600);
  const m = Math.floor((abs % 3600) / 60);
  return `UTC${sign}${h}${m ? ':' + String(m).padStart(2, '0') : ''}`;
}

export function resolveZone(tz?: DeviceTz | null): ResolvedZone {
  if (tz?.iana && validIana(tz.iana)) return { timeZone: tz.iana };
  if (tz?.posix) {
    const off = posixStdOffsetSec(tz.posix);
    if (off !== undefined) return { timeZone: 'UTC', offsetSec: off, hint: offsetHint(off) };
  }
  return {};
}

// x value -> epoch ms. Epoch seconds, or ms when >= 1e11 (year 5138 in s).
export function toEpochMs(x: number): number {
  return Math.abs(x) >= 1e11 ? x : x * 1000;
}

type FmtKey = Exclude<XFormat, 'epoch'> | 'full' | 'dateFull' | 'datetimeFull' | 'monthYear';

const OPTS: Record<FmtKey, Intl.DateTimeFormatOptions> = {
  time: { hour: '2-digit', minute: '2-digit', hourCycle: 'h23' },
  date: { day: '2-digit', month: '2-digit' },
  datetime: { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit', hourCycle: 'h23' },
  dom: { day: 'numeric' },
  month: { month: 'short' },
  full: {
    year: 'numeric', month: '2-digit', day: '2-digit',
    hour: '2-digit', minute: '2-digit', second: '2-digit', hourCycle: 'h23',
  },
  // table cells (SES-LIB-016)
  dateFull: { year: 'numeric', month: '2-digit', day: '2-digit' },
  datetimeFull: { year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', hourCycle: 'h23' },
  monthYear: { month: 'short', year: 'numeric' },
};

const fmtCache = new Map<string, Intl.DateTimeFormat>();

function formatter(zone: ResolvedZone, key: FmtKey, locale?: string): Intl.DateTimeFormat {
  const id = `${locale || ''}|${zone.timeZone || ''}|${key}`;
  let f = fmtCache.get(id);
  if (!f) {
    f = new Intl.DateTimeFormat(locale, { ...OPTS[key], timeZone: zone.timeZone });
    fmtCache.set(id, f);
  }
  return f;
}

// "epoch" picks time for a span under 36 h, else date.
export function effectiveFormat(fmt: XFormat, spanSec?: number): Exclude<XFormat, 'epoch'> {
  if (fmt !== 'epoch') return fmt;
  return spanSec !== undefined && spanSec < 36 * 3600 ? 'time' : 'date';
}

// Axis tick / category label. Without a format (or for a non-number) the
// value is shown as it is: millis()-style x stays a raw number.
export function formatX(
  x: unknown,
  fmt: XFormat | undefined,
  zone: ResolvedZone,
  spanSec?: number,
  locale?: string,
): string {
  if (!fmt || typeof x !== 'number' || !Number.isFinite(x)) return String(x ?? '');
  const ms = toEpochMs(x) + (zone.offsetSec ?? 0) * 1000;
  return formatter(zone, effectiveFormat(fmt, spanSec), locale).format(new Date(ms));
}

// Tooltip title: the full local date and time (with the "UTC±N" hint when the
// zone is only a fixed offset).
export function formatFull(x: unknown, fmt: XFormat | undefined, zone: ResolvedZone, locale?: string): string {
  if (!fmt || fmt === 'dom' || fmt === 'month' || typeof x !== 'number' || !Number.isFinite(x)) {
    return formatX(x, fmt, zone, undefined, locale);
  }
  const ms = toEpochMs(x) + (zone.offsetSec ?? 0) * 1000;
  const s = formatter(zone, 'full', locale).format(new Date(ms));
  return zone.hint ? `${s} (${zone.hint})` : s;
}

// Time of day: seconds since local midnight <-> "HH:MM". No Date, no zone.
export function secOfDayToHHMM(sec: unknown): string {
  const n = typeof sec === 'number' ? sec : typeof sec === 'string' ? parseInt(sec, 10) : NaN;
  if (!Number.isFinite(n)) return '';
  const s = ((Math.floor(n) % 86400) + 86400) % 86400;
  return `${String(Math.floor(s / 3600)).padStart(2, '0')}:${String(Math.floor((s % 3600) / 60)).padStart(2, '0')}`;
}

export function hhmmToSecOfDay(v: string): number | undefined {
  const m = /^(\d{1,2}):(\d{2})(?::(\d{2}))?$/.exec(v.trim());
  if (!m) return undefined;
  const h = Number(m[1]);
  const min = Number(m[2]);
  const s = Number(m[3] || 0);
  if (h > 23 || min > 59 || s > 59) return undefined;
  return h * 3600 + min * 60 + s;
}

// ── Table cells (SES-LIB-016) ──────────────────────────────────────────────

// Duration -> the two largest units: "3d 4h", "2h 05m", "45m 10s", "12s", "0s".
// unit: what the value counts — seconds (default), ms or minutes.
export function formatDuration(v: unknown, unit?: string): string {
  const n = typeof v === 'number' ? v : typeof v === 'string' ? parseFloat(v) : NaN;
  if (!Number.isFinite(n)) return String(v ?? '');
  const scale = unit === 'ms' ? 0.001 : unit === 'min' ? 60 : 1;
  let s = Math.round(Math.abs(n * scale));
  const sign = n < 0 && s > 0 ? '−' : '';
  const d = Math.floor(s / 86400);
  s -= d * 86400;
  const h = Math.floor(s / 3600);
  s -= h * 3600;
  const m = Math.floor(s / 60);
  s -= m * 60;
  const p2 = (x: number) => String(x).padStart(2, '0');
  if (d > 0) return `${sign}${d}d ${h}h`;
  if (h > 0) return `${sign}${h}h ${p2(m)}m`;
  if (m > 0) return `${sign}${m}m ${p2(s)}s`;
  return `${sign}${s}s`;
}

export type CellTimeType = 'date' | 'time' | 'datetime' | 'duration';

export function isCellTimeType(t: string): t is CellTimeType {
  return t === 'date' || t === 'time' || t === 'datetime' || t === 'duration';
}

// A dated table cell in the device zone. date (fmt "month" -> "Oct 2026"),
// time (fmt "tod": the value is seconds since midnight), datetime, duration.
// Non-numbers show as they are.
export function formatCellTime(
  val: unknown,
  type: CellTimeType,
  fmt: string | undefined,
  zone: ResolvedZone,
  locale?: string,
): string {
  if (val === undefined || val === null || val === '') return '';
  if (type === 'duration') return formatDuration(val, fmt);
  if (type === 'time' && fmt === 'tod') return secOfDayToHHMM(val) || String(val);
  const n = typeof val === 'number' ? val : typeof val === 'string' ? parseFloat(val) : NaN;
  if (!Number.isFinite(n)) return String(val);
  const key: FmtKey =
    type === 'time' ? 'time' : type === 'datetime' ? 'datetimeFull' : fmt === 'month' ? 'monthYear' : 'dateFull';
  const ms = toEpochMs(n) + (zone.offsetSec ?? 0) * 1000;
  return formatter(zone, key, locale).format(new Date(ms));
}
