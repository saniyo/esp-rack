// SES-LIB-029 [host]: the picker's zone math and 24-hour formatting.
import { describe, expect, it } from '@jest/globals';

import { fmtDate, fmtTime, monthGrid, wallParts, wallToEpoch, zoneLabel } from './dateTimeParts';

describe('date/time parts', () => {
  it('reads wall clock parts in a zone', () => {
    const w = wallParts(0, { timeZone: 'UTC' });
    expect(fmtDate(w)).toBe('1970-01-01');
    expect(fmtTime(w)).toBe('00:00:00');
    expect(fmtTime(wallParts(13 * 3600 + 5 * 60 + 9, { timeZone: 'UTC' }))).toBe('13:05:09');  // 24-hour
  });
  it('round-trips through the device zone, summer and winter', () => {
    const kyiv = { timeZone: 'Europe/Kyiv' };
    for (const t of [1719835200, 1704110400]) expect(wallToEpoch(wallParts(t, kyiv), kyiv)).toBe(t);
    expect(fmtTime(wallParts(1719835200, kyiv))).toBe('15:00:00');  // UTC+3 in July
    const fixed = { timeZone: 'UTC', offsetSec: 7200, hint: 'UTC+2' };
    expect(wallToEpoch(wallParts(1704110400, fixed), fixed)).toBe(1704110400);
    expect(zoneLabel(fixed)).toBe('UTC+2');
  });
  it('lays out a month, Monday first', () => {
    const g = monthGrid(2026, 10);  // 1 Oct 2026 is a Thursday
    expect(g.slice(0, 4)).toEqual([0, 0, 0, 1]);
    expect(g.filter((d) => d > 0).length).toBe(31);
  });
});
