// [host]: a trend's series (lines=) and the signed area under one
// (.signedArea in web/Form.h).
import { describe, expect, it } from '@jest/globals';

import { parseFieldOptions } from '../../utils/fieldParser';
import { areaColor, parseLineConfigs, signedAreaCurve, signedAreaOffset, tooltipEntries } from './lineConfigs';

describe('signed area', () => {
  // web::Trend("load").line("p", "#555555", "monotone", "Load").signedArea("#1e88e5", "#eb6834")
  //     .line("pv", "#ff9800", "monotone").zero()
  const o =
    'trend;r;mode=lineChart;xAxis=timestamp;lines=p:color=#555555,type=monotone,name=Load,ap=#1e88e5,an=#eb6834;' +
    'pv:color=#ff9800,type=monotone;maxPoints=100;y0=1';

  it('keeps the area colours on their line', () => {
    const p = parseFieldOptions('load', o);
    const lines = parseLineConfigs(p.optionMap.lines?.value as string, {});
    expect(lines).toHaveLength(2);
    expect(lines[0]).toMatchObject({ dataKey: 'p', color: '#555555', readableLabel: 'Load', areaPos: '#1e88e5', areaNeg: '#eb6834' });
    expect(lines[1].areaPos).toBeUndefined();
    expect(lines[1].areaNeg).toBeUndefined();
    expect(p.optionMap.y0?.value).toBe(true);
  });

  it('takes theme palette keys, and one colour for both sides', () => {
    const [a] = parseLineConfigs('p:color=#555,type=monotone,ap=info,an=warning', {});
    expect(a.areaPos).toBe('info');
    const palette = { info: { main: '#0288d1' }, warning: { main: '#ed6c02' }, mode: 'dark' };
    expect(areaColor(a.areaPos, palette)).toBe('#0288d1');
    expect(areaColor(a.areaNeg, palette)).toBe('#ed6c02');
    expect(areaColor('#1e88e5', palette)).toBe('#1e88e5');
    expect(areaColor('mode', palette)).toBe('mode');  // not a palette colour
    expect(areaColor(undefined, palette)).toBeUndefined();
    const [b] = parseLineConfigs('p:color=#555,type=monotone,ap=#1e88e5', {});
    expect(b.areaNeg).toBe('#1e88e5');
  });

  it('puts the gradient stop at 0', () => {
    expect(signedAreaOffset([{ p: 3 }, { p: -1 }], 'p')).toBe(0.75);
    expect(signedAreaOffset([{ p: 3 }, { p: 1 }], 'p')).toBe(1);   // all above: one colour
    expect(signedAreaOffset([{ p: -3 }, { p: -1 }], 'p')).toBe(0); // all below
    expect(signedAreaOffset([], 'p')).toBe(1);
    expect(signedAreaOffset([{ p: 'x' }, { q: 5 }, { p: -2 }, { p: 2 }], 'p')).toBe(0.5);
  });

  it('draws the area on a curve that stays within the data extremes', () => {
    expect(signedAreaCurve('linear')).toBe('linear');
    expect(signedAreaCurve('monotone')).toBe('monotone');
    expect(signedAreaCurve('stepAfter')).toBe('stepAfter');
    expect(signedAreaCurve('basis')).toBe('monotone');
    expect(signedAreaCurve('basisOpen')).toBe('monotone');
    expect(signedAreaCurve('natural')).toBe('monotone');
  });

  it('leaves the signed area out of the tooltip', () => {
    // recharts hands our tooltip the Area too (type 'none', same dataKey)
    const payload = [
      { dataKey: 'p', type: 'none', name: 'p', color: 'url(#sa-load-p)' },
      { dataKey: 'p', name: 'Load', color: '#555555' },
      { dataKey: 'q', name: 'Q', color: '#111111' },
    ];
    expect(tooltipEntries(payload, null).map((e) => e.name)).toEqual(['Load', 'Q']);
    expect(tooltipEntries(payload, 'p').map((e) => e.name)).toEqual(['Load']);
    expect(tooltipEntries([payload[0]], null)).toEqual([]);
  });
});
