import { describe, expect, it } from '@jest/globals';
import { bucketWord, planSeries, planWindow, timeTicks } from './historyPlan';

const D = 1761264000;  // a UTC midnight
const utcDay = (t: number) => t - (t % 86400);
const plus2Day = (t: number) => t - ((t + 7200) % 86400);  // UTC+2 midnights
const cells = [
  { bucket: 300, keep: 3 * 86400 },
  { bucket: 3600, keep: 60 * 86400 },
  { bucket: 86400, keep: 730 * 86400 },
];
const balance = [
  { bucket: 3600, keep: 7 * 86400 },
  { bucket: 86400, keep: 365 * 86400 },
];

describe('history window plan', () => {
  it('shows a fresh device its first minutes on every period', () => {
    const now = D + 10 * 3600 + 38 * 60;
    const oldest = D + 10 * 3600 + 25 * 60;
    ['24h', '7d', '30d', '1y'].forEach((period) => {
      const p = planWindow({ period: period as any, now, oldest, tiers: cells, localDays: false, dayStart: utcDay });
      expect(p.tier).toBe(0);
      expect(p.fromEff).toBe(oldest);
      expect(p.to).toBe(D + 10 * 3600 + 35 * 60);
      expect(p.clamped).toBe(true);
      expect(p.empty).toBeUndefined();
    });
  });

  it('has nothing to show before the first record', () => {
    const now = D + 10 * 3600;
    expect(planWindow({ period: '7d', now, oldest: 0, tiers: balance, localDays: false, dayStart: utcDay }).empty).toBe('none');
    const day = planWindow({ period: 'date', day: D - 86400, now, oldest: D + 3600, tiers: balance, localDays: false, dayStart: utcDay });
    expect(day.empty).toBe('before');
  });

  it('reads 7 days from the day tier, whole days to today 00:00', () => {
    const now = D + 14 * 3600 + 35 * 60;
    const p = planWindow({ period: '7d', now, oldest: D - 300 * 86400, tiers: balance, localDays: false, dayStart: utcDay });
    expect(p.tier).toBe(1);
    expect(p.to).toBe(D);
    expect(p.from).toBe(D - 7 * 86400);
    expect(p.openEnd).toBe(true);
    expect(p.clamped).toBe(false);
  });

  it('reads 24 h from the hourly tier to the last closed hour', () => {
    const now = D + 14 * 3600 + 35 * 60;
    const p = planWindow({ period: '24h', now, oldest: D - 300 * 86400, tiers: balance, localDays: false, dayStart: utcDay });
    expect(p.tier).toBe(0);
    expect(p.to).toBe(D + 14 * 3600);
    expect(p.fromEff).toBe(D + 14 * 3600 - 86400);
  });

  it('puts local-day buckets on local midnight', () => {
    const now = D + 14 * 3600;
    const p = planWindow({ period: '7d', now, oldest: D - 300 * 86400, tiers: balance, localDays: true, dayStart: plus2Day });
    expect(p.to).toBe(D - 7200);  // 00:00 at UTC+2
    expect(p.from).toBe(D - 7200 - 7 * 86400);
  });

  it('reads a past day whole, today to the last closed bucket', () => {
    const now = D + 14 * 3600 + 35 * 60;
    const past = planWindow({ period: 'date', day: D - 86400, now, oldest: D - 300 * 86400, tiers: cells, localDays: false, dayStart: utcDay });
    expect(past.from).toBe(D - 86400);
    expect(past.to).toBe(D);
    expect(past.openEnd).toBe(false);
    const today = planWindow({ period: 'date', day: D, now, oldest: D - 300 * 86400, tiers: cells, localDays: false, dayStart: utcDay });
    expect(today.to).toBe(D + 14 * 3600 + 35 * 60);
    expect(today.openEnd).toBe(true);
  });

  it('reads a time series coarse enough, from the right tier', () => {
    const battery = [
      { bucket: 60, keep: 2 * 86400 },
      { bucket: 900, keep: 60 * 86400 },
      { bucket: 3600, keep: 365 * 86400 },
    ];
    const now = D + 14 * 3600 + 35 * 60;
    const old = D - 300 * 86400;
    const day = planSeries({ period: '24h', now, oldest: old, tiers: battery, localDays: false, dayStart: utcDay });
    expect(day.res).toBe(300);
    expect(day.plan.tier).toBe(0);
    const month = planSeries({ period: '30d', now, oldest: old, tiers: battery, localDays: false, dayStart: utcDay });
    expect(month.res).toBe(3 * 3600);
    expect(month.plan.tier).toBe(2);
    const fresh = planSeries({ period: '1y', now, oldest: now - 600, tiers: battery, localDays: false, dayStart: utcDay });
    expect(fresh.res).toBe(60);
    expect(fresh.plan.tier).toBe(0);
  });

  it('puts ticks on round wall-clock times', () => {
    const ticks = timeTicks(D + 100, D + 86400, 7200);
    expect(ticks.length).toBeGreaterThan(3);
    ticks.forEach((t) => expect((t + 7200) % (4 * 3600) === 0 || (t + 7200) % 3600 === 0).toBe(true));
  });

  it('names buckets', () => {
    expect(bucketWord(300)).toBe('5-min');
    expect(bucketWord(3600)).toBe('hourly');
    expect(bucketWord(86400)).toBe('daily');
  });
});
