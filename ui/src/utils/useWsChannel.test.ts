// Inbound coalescing and outbound write merging of useWsChannel.
import { describe, expect, it } from '@jest/globals';

import { coalescePayload, mergeOutbound } from './useWsChannel';

describe('coalescePayload', () => {
  it('concatenates increments, keeps the newest full set', () => {
    const full = (k: string) => k === 'cells' || k === 'rows';
    const a = { v: 1, pts: [{ timestamp: 1 }], cells: [{ c: '01', mv: 3300 }], rows: [{ id: 1 }] };
    const b = { v: 2, pts: [{ timestamp: 2 }], cells: [{ c: '01', mv: 3301 }], rows: [{ id: 1 }] };
    const out = coalescePayload(coalescePayload(undefined, a, full), b, full);
    expect(out.v).toBe(2);
    expect(out.pts).toEqual([{ timestamp: 1 }, { timestamp: 2 }]);
    expect(out.cells).toEqual([{ c: '01', mv: 3301 }]);
    expect(out.rows).toEqual([{ id: 1 }]);
  });

  it('concatenates every array without a full-set list (as before)', () => {
    const out = coalescePayload(coalescePayload(undefined, { a: [1] }), { a: [2] });
    expect(out.a).toEqual([1, 2]);
  });
});

describe('mergeOutbound', () => {
  it('keeps every key written in one throttle window', () => {
    const a = { type: 'p', origin_id: 'x', p: { min_mv: 3000 } };
    const b = { type: 'p', origin_id: 'x', p: { max_mv: 3600 } };
    expect(mergeOutbound(a, b)).toEqual({ type: 'p', origin_id: 'x', p: { min_mv: 3000, max_mv: 3600 } });
    expect(mergeOutbound(undefined, b)).toBe(b);
  });
});
