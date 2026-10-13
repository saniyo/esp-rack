// SES-LIB-015 [host]: the option strings the C++ builders emit (web/Form.h),
// parsed back by the frontend. The strings are exactly what Form.h writes for
// the calls in the comments (Arduino String(double) prints two decimals);
// keep them in step with Form.h / FieldVocab.h.
import { describe, expect, it } from '@jest/globals';

import { parseFieldOptions, RefLineSpec, ThresholdStep } from './fieldParser';
import {
  formatFull,
  formatX,
  hhmmToSecOfDay,
  posixStdOffsetSec,
  resolveZone,
  secOfDayToHHMM,
  toEpochMs,
} from '../../../utils/deviceTime';

describe('Progress', () => {
  // web::Progress("today", 3.5).max(10).maxKey("plan_max").markerKey("plan_pct")
  //     .unit("kWh").format("0.0").thresholds("0:error,60:warning,95:success")
  const o = 'progress;r;mn=0.00;mx=10.00;maxKey=plan_max;markerKey=plan_pct;unit=kWh;f=0.0;thr=0:error,60:warning,95:success';

  it('parses type, range and sibling keys', () => {
    const p = parseFieldOptions('today', o);
    expect(p.finalType).toBe('progress');
    expect(p.optionMap.r?.value).toBe(true);
    expect(p.optionMap.mn?.value).toBe(0);
    expect(p.optionMap.mx?.value).toBe(10);
    expect(p.optionMap.maxKey?.value).toBe('plan_max');
    expect(p.optionMap.markerKey?.value).toBe('plan_pct');
    expect(p.optionMap.unit?.value).toBe('kWh');
  });

  it('sorts threshold steps', () => {
    const p = parseFieldOptions('today', 'progress;r;thr=95:success,0:error,60:warning');
    expect(p.optionMap.thr?.value as ThresholdStep[]).toEqual([
      { pct: 0, color: 'error' },
      { pct: 60, color: 'warning' },
      { pct: 95, color: 'success' },
    ]);
  });
});

describe('Trend', () => {
  // web::Trend("days").mode("barChart").xAxis("day").category().xFormat("dom")
  //     .stack("on_min", "off_min").refLine("plan_min", "#888", "Plan")
  //     .line("on_min", "#f00", "monotone")
  const o =
    'trend;r;mode=barChart;xAxis=day;lines=on_min:color=#f00,type=monotone;maxPoints=100;' +
    'xFmt=dom;cat;stack=on_min+off_min;refl=plan_min:#888:Plan';

  it('parses the new options next to the old ones', () => {
    const p = parseFieldOptions('days', o);
    expect(p.finalType).toBe('trend');
    expect(p.optionMap.xFmt?.value).toBe('dom');
    expect(p.optionMap.cat?.value).toBe(true);
    expect(p.optionMap.stack?.value).toEqual([['on_min', 'off_min']]);
    expect(p.optionMap.refl?.value as RefLineSpec[]).toEqual([{ key: 'plan_min', color: '#888', label: 'Plan' }]);
    expect(p.optionMap.lines?.value).toBe('on_min:color=#f00,type=monotone');
    expect(p.optionMap.maxPoints?.value).toBe(100);
  });

  it('keeps several stacks and reference lines', () => {
    const p = parseFieldOptions('t', 'trend;r;stack=a+b|c+d+e;refl=x:#f00:|y::Limit');
    expect(p.optionMap.stack?.value).toEqual([['a', 'b'], ['c', 'd', 'e']]);
    expect(p.optionMap.refl?.value).toEqual([
      { key: 'x', color: '#f00', label: '' },
      { key: 'y', color: '', label: 'Limit' },
    ]);
  });

  it('without xFmt nothing new is set (old trends unchanged)', () => {
    // web::Trend("rate").line("rate_bps", "#1e88e5", "monotone").maxPoints(60) (the template)
    const p = parseFieldOptions('rate', 'trend;r;mode=lineChart;xAxis=timestamp;lines=rate_bps:color=#1e88e5,type=monotone;maxPoints=60');
    expect(p.optionMap.xFmt).toBeUndefined();
    expect(p.optionMap.cat).toBeUndefined();
  });
});

describe('DateTime time of day', () => {
  it('parses dtmode=tod', () => {
    // web::DateTime("on_at", 81000).timeOfDay()
    const p = parseFieldOptions('on_at', 'datetime;rw;dtmode=tod');
    expect(p.optionMap.dtmode?.value).toBe('tod');
  });

  it('round-trips seconds of day without any timezone', () => {
    expect(secOfDayToHHMM(81000)).toBe('22:30');
    expect(secOfDayToHHMM(0)).toBe('00:00');
    expect(secOfDayToHHMM(86399)).toBe('23:59');
    expect(hhmmToSecOfDay('22:30')).toBe(81000);
    expect(hhmmToSecOfDay('07:05:09')).toBe(7 * 3600 + 5 * 60 + 9);
    expect(hhmmToSecOfDay('24:00')).toBeUndefined();
    expect(hhmmToSecOfDay('')).toBeUndefined();
  });
});

describe('device timezone', () => {
  // The test runs with the process in America/New_York (package.json
  // "test" or TZ env); the device is in Kyiv.
  const kyiv = resolveZone({ iana: 'Europe/Kyiv', posix: 'EET-2EEST,M3.5.0/3,M10.5.0/4' });

  it('keeps raw x without a format (millis() must not become 1970 dates)', () => {
    expect(formatX(123456, undefined, kyiv)).toBe('123456');
    expect(formatX('Mon', 'dom', kyiv)).toBe('Mon');
  });

  it('formats across the DST change in the device zone', () => {
    // 2026-03-29 00:30Z = 02:30 EET; 01:30Z = 04:30 EEST (Kyiv springs forward).
    const before = Date.UTC(2026, 2, 29, 0, 30) / 1000;
    const after = Date.UTC(2026, 2, 29, 1, 30) / 1000;
    expect(formatX(before, 'time', kyiv, undefined, 'en-GB')).toBe('02:30');
    expect(formatX(after, 'time', kyiv, undefined, 'en-GB')).toBe('04:30');
    expect(formatX(after * 1000, 'time', kyiv, undefined, 'en-GB')).toBe('04:30');  // ms input
    expect(formatX(before, 'dom', kyiv, undefined, 'en-GB')).toBe('29');
  });

  it('epoch picks time under 36 h, else date', () => {
    const t = Date.UTC(2026, 9, 13, 12, 0) / 1000;  // 15:00 in Kyiv (EEST)
    expect(formatX(t, 'epoch', kyiv, 3600, 'en-GB')).toBe('15:00');
    expect(formatX(t, 'epoch', kyiv, 10 * 86400, 'en-GB')).toBe('13/10');
  });

  it('falls back to the POSIX standard offset with a hint', () => {
    expect(posixStdOffsetSec('EET-2EEST,M3.5.0/3,M10.5.0/4')).toBe(7200);
    expect(posixStdOffsetSec('<+0530>-5:30')).toBe(19800);
    expect(posixStdOffsetSec('EST5EDT')).toBe(-18000);
    const z = resolveZone({ posix: 'EET-2' });
    expect(z.hint).toBe('UTC+2');
    const t = Date.UTC(2026, 9, 13, 12, 0) / 1000;
    expect(formatX(t, 'time', z, undefined, 'en-GB')).toBe('14:00');
    expect(formatFull(t, 'time', z, 'en-GB')).toContain('(UTC+2)');
  });

  it('uses the browser zone when nothing is known', () => {
    expect(resolveZone(undefined)).toEqual({});
    expect(resolveZone({ iana: 'Not/AZone' })).toEqual({});
  });

  it('detects ms', () => {
    expect(toEpochMs(1_790_000_000)).toBe(1_790_000_000_000);
    expect(toEpochMs(1_790_000_000_000)).toBe(1_790_000_000_000);
  });
});
