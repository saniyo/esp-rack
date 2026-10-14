// SES-LIB-029: wall-clock parts of an instant in the device's zone and back,
// for the date/time picker. 24-hour, no library: Intl does the zone rules.
import { ResolvedZone } from '../../../utils/deviceTime';

export interface WallParts {
  y: number;
  mo: number;  // 1..12
  d: number;
  h: number;
  mi: number;
  s: number;
}

const fmtCache = new Map<string, Intl.DateTimeFormat>();
const dtf = (zone: ResolvedZone): Intl.DateTimeFormat => {
  const key = zone.timeZone ?? '';
  let f = fmtCache.get(key);
  if (!f) {
    f = new Intl.DateTimeFormat('en-GB', {
      timeZone: zone.timeZone,
      hourCycle: 'h23',
      year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', second: '2-digit',
    });
    fmtCache.set(key, f);
  }
  return f;
};

export function wallParts(sec: number, zone: ResolvedZone): WallParts {
  const ms = sec * 1000 + (zone.offsetSec ?? 0) * 1000;
  const p: Record<string, number> = {};
  dtf(zone).formatToParts(new Date(ms)).forEach((x) => {
    if (x.type !== 'literal') p[x.type] = Number(x.value);
  });
  return { y: p.year, mo: p.month, d: p.day, h: p.hour % 24, mi: p.minute, s: p.second };
}

const utcOf = (w: WallParts): number => Date.UTC(w.y, w.mo - 1, w.d, w.h, w.mi, w.s) / 1000;

// The instant whose wall clock in `zone` reads w (twice round for DST).
export function wallToEpoch(w: WallParts, zone: ResolvedZone): number {
  let t = utcOf(w);
  for (let k = 0; k < 2; ++k) t -= utcOf(wallParts(t, zone)) - utcOf(w);
  return t;
}

const p2 = (n: number) => String(n).padStart(2, '0');
export const fmtDate = (w: WallParts) => `${w.y}-${p2(w.mo)}-${p2(w.d)}`;
export const fmtTime = (w: WallParts, seconds = true) => `${p2(w.h)}:${p2(w.mi)}${seconds ? `:${p2(w.s)}` : ''}`;

// "Europe/Kyiv", "UTC+2" or "browser time": what the picker's times are in.
export const zoneLabel = (zone: ResolvedZone): string => zone.hint ?? zone.timeZone ?? 'browser time';

// 6 weeks x 7 days of the month, Monday first; 0 = outside the month.
export function monthGrid(y: number, mo: number): number[] {
  const first = new Date(Date.UTC(y, mo - 1, 1)).getUTCDay();  // 0 = Sunday
  const lead = (first + 6) % 7;
  const days = new Date(Date.UTC(y, mo, 0)).getUTCDate();
  return Array.from({ length: 42 }, (_, i) => (i >= lead && i < lead + days ? i - lead + 1 : 0));
}
