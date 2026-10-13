// SES-LIB-015 [host]: the option strings the C++ builders emit (web/Form.h),
// parsed back by the frontend. The strings are exactly what Form.h writes for
// the calls in the comments (Arduino String(double) prints two decimals);
// keep them in step with Form.h / FieldVocab.h.
import { describe, expect, it } from '@jest/globals';

import { parseFieldOptions, RefLineSpec, ThresholdStep } from './fieldParser';
import {
  formatCellTime,
  formatDuration,
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

// ── SES-LIB-016 ──
describe('Message levelKey / Action clientTime', () => {
  it('parses levelKey', () => {
    // web::Message("m_state", "…").level("info").levelKey("state_level")
    const p = parseFieldOptions('m_state', 'message;r;level=info;levelKey=state_level');
    expect(p.optionMap.levelKey?.value).toBe('state_level');
    expect(p.optionMap.level?.value).toBe('info');
  });

  it('parses clientTime', () => {
    // web::Action("a_clock").ref("system.setTime").withClientTime("epoch", "ms")
    const p = parseFieldOptions('a_clock', 'action;rw;ref=system.setTime;clientTime=epoch,ms');
    expect(p.optionMap.clientTime?.value).toEqual(['epoch', 'ms']);
    expect(p.optionMap.ref?.value).toBe('system.setTime');
    const q = parseFieldOptions('a', 'action;rw;clientTime=t');
    expect(q.optionMap.clientTime?.value).toEqual(['t']);
  });
});

describe('table cells in the device zone', () => {
  const kyiv = resolveZone({ iana: 'Europe/Kyiv' });
  // 2026-03-29 01:30Z = 04:30 EEST (after the spring-forward)
  const t = Date.UTC(2026, 2, 29, 1, 30) / 1000;

  it('date / month / datetime / time', () => {
    expect(formatCellTime(t, 'date', undefined, kyiv, 'en-GB')).toBe('29/03/2026');
    expect(formatCellTime(t, 'date', 'month', kyiv, 'en-GB')).toBe('Mar 2026');
    expect(formatCellTime(t, 'datetime', undefined, kyiv, 'en-GB')).toBe('29/03/2026, 04:30');
    expect(formatCellTime(t, 'time', undefined, kyiv, 'en-GB')).toBe('04:30');
    expect(formatCellTime(t * 1000, 'time', undefined, kyiv, 'en-GB')).toBe('04:30');  // ms
  });

  it('time of day has no zone', () => {
    expect(formatCellTime(81000, 'time', 'tod', kyiv, 'en-GB')).toBe('22:30');
  });

  it('keeps non-numbers and empties', () => {
    expect(formatCellTime('n/a', 'date', undefined, kyiv)).toBe('n/a');
    expect(formatCellTime(null, 'date', undefined, kyiv)).toBe('');
    expect(formatCellTime('', 'duration', undefined, kyiv)).toBe('');
  });

  it('durations: two largest units', () => {
    expect(formatDuration(0)).toBe('0s');
    expect(formatDuration(45)).toBe('45s');
    expect(formatDuration(2710)).toBe('45m 10s');
    expect(formatDuration(7500)).toBe('2h 05m');
    expect(formatDuration(3600)).toBe('1h 00m');
    expect(formatDuration(3 * 86400 + 4 * 3600 + 59)).toBe('3d 4h');
    expect(formatDuration(-90)).toBe('−1m 30s');
    expect(formatDuration(1500, 'ms')).toBe('2s');
    expect(formatDuration(125, 'min')).toBe('2h 05m');
    expect(formatCellTime(125, 'duration', 'min', kyiv)).toBe('2h 05m');
  });
});
