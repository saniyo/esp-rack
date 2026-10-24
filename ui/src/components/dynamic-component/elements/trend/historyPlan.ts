// historyPlan: which records a history period reads (pure; device time).
// Order matters: the period is clamped to the start of recording BEFORE the
// tier is chosen, so a fresh device shows its first minutes on every period,
// never an empty axis with a sliver at the right.
import { HistTierInfo } from '../../../../framework/history/historyClient';

export type HistPeriod = 'live' | '24h' | '7d' | '30d' | '1y' | 'date';
export const ALL_PERIODS: HistPeriod[] = ['live', '24h', '7d', '30d', '1y', 'date'];
export const PERIOD_SPAN: Record<string, number> = { '24h': 86400, '7d': 7 * 86400, '30d': 30 * 86400, '1y': 365 * 86400 };

export interface PlanInput {
  period: Exclude<HistPeriod, 'live'>;
  day?: number;            // 'date': the local midnight that starts it
  now: number;             // device epoch s
  oldest: number;          // the series' oldest record (0 = none)
  tiers: HistTierInfo[];
  localDays: boolean;      // whole-day tiers start at local midnight
  dayStart: (t: number) => number;  // the local midnight at or before t
}

export type EmptyReason = 'none' | 'before' | 'noClosed';
export interface Plan {
  tier: number;
  bucket: number;
  from: number;     // the period's start (after alignment, before the clamp)
  fromEff: number;  // what is read: from, clamped to the recording, at a bucket start
  to: number;       // exclusive: the end of the last closed bucket (a past day: its end)
  dayEnd?: number;  // 'date': the day's end
  openEnd: boolean; // `to` is the start of the open bucket (the period reaches now)
  clamped: boolean; // the period starts before the recording
  empty?: EmptyReason;
}

// The local midnight after the day that starts at `day` (DST days: 23/25 h).
export const nextDay = (day: number, dayStart: (t: number) => number) => dayStart(day + 36 * 3600);

export function bucketStart(t: number, bucket: number, localDay: boolean, dayStart: (t: number) => number): number {
  if (!bucket) return t;
  if (localDay && bucket % 86400 === 0) return dayStart(t);
  return t - (t % bucket);
}

// A tier holds t safely when t is a bucket inside its retention (the oldest
// record of a full ring may already be gone).
const covers = (tier: HistTierInfo, t: number, now: number) => t >= now - tier.keep + tier.bucket;

export function planWindow(p: PlanInput): Plan {
  const span = PERIOD_SPAN[p.period] || 0;
  const dayEnd = p.period === 'date' && p.day !== undefined ? nextDay(p.day, p.dayStart) : undefined;
  const rawFrom = p.period === 'date' ? p.day ?? p.now : p.now - span;
  const n = p.tiers.length;
  let k = 0;
  while (k < n - 1 && !covers(p.tiers[k], Math.max(rawFrom, p.oldest), p.now)) ++k;

  for (;;) {
    const tier = p.tiers[k];
    const local = p.localDays && tier.bucket % 86400 === 0;
    const open = bucketStart(p.now, tier.bucket, local, p.dayStart);  // the open bucket's start
    let from: number;
    let to: number;
    if (p.period === 'date') {
      from = rawFrom;
      to = Math.min(dayEnd ?? p.now, open);
    } else {
      to = open;
      from = bucketStart(to - span, tier.bucket, local, p.dayStart);
    }
    const clamped = p.oldest > from;
    const fromEff = bucketStart(Math.max(from, p.oldest), tier.bucket, local, p.dayStart);
    if (k < n - 1 && !covers(tier, fromEff, p.now)) {
      ++k;
      continue;
    }
    const plan: Plan = { tier: k, bucket: tier.bucket, from, fromEff, to, dayEnd, openEnd: to === open, clamped };
    if (!p.oldest) plan.empty = 'none';
    else if ((dayEnd ?? to) <= p.oldest) plan.empty = 'before';
    else if (fromEff >= to) plan.empty = 'noClosed';
    return plan;
  }
}

// "5-min", "hourly", "daily", "15-s"
export function bucketWord(bucket: number): string {
  if (bucket === 3600) return 'hourly';
  if (bucket === 86400) return 'daily';
  if (bucket % 3600 === 0) return `${bucket / 3600}-h`;
  if (bucket % 60 === 0) return `${bucket / 60}-min`;
  return `${bucket}-s`;
}
