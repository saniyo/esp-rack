// SES-LIB-029 [host]: layout segments, grid columns, the 0 value.
import { describe, expect, it } from '@jest/globals';

import {
  BRK_TYPE, COLS_TYPE, fullOf, gridColumnFor, gridTemplateFor, markerFromRaw, numberInputValue, READ_ONLY_MIN_W,
  READ_ONLY_TILE_SX, segmentFields, spanOf,
} from './formLayout';

const f = (label: string, type = 'number') => ({ label, type });

describe('read-only tiles (Status tabs)', () => {
  it('makes a read-only text a tile, keeps tables and trends full', () => {
    expect(gridColumnFor('text', undefined, false, undefined, true)).toBe('auto');
    expect(gridColumnFor('number', undefined, false, undefined, true)).toBe('auto');
    expect(gridColumnFor('table', undefined, false, undefined, true)).toBe('1 / -1');
    expect(gridColumnFor('trend', undefined, false, undefined, true)).toBe('1 / -1');
    expect(gridColumnFor('text', undefined, true, undefined, true)).toBe('1 / -1');  // fullRow wins
  });
  it('flows read-only segments in narrower columns, without dividers', () => {
    expect(gridTemplateFor(undefined, READ_ONLY_MIN_W)).toBe('repeat(auto-fill, minmax(220px, 1fr))');
    expect(gridTemplateFor(2, READ_ONLY_MIN_W)).toEqual({ xs: '1fr', sm: 'repeat(2, 1fr)', md: 'repeat(2, 1fr)' });
    expect(READ_ONLY_TILE_SX['& .MuiDivider-root']).toEqual({ display: 'none' });
  });
});

describe('form layout', () => {
  it('cuts the field stream at the markers', () => {
    const fields = [
      f('a'), markerFromRaw({ t: 'cols', n: 2 }, 1)!, f('b'), f('c'), f('d'),
      markerFromRaw({ t: 'cols', n: 3 }, 5)!, f('e'), markerFromRaw({ t: 'brk' }, 7)!, f('note', 'text'),
      markerFromRaw({ t: 'brk' }, 9)!,
    ];
    const segs = segmentFields(fields, (x) => x);
    expect(segs.map((s) => [s.cols, s.cells.map((c) => c.label).join('')])).toEqual([
      [undefined, 'a'], [2, 'bcd'], [3, 'e'], [undefined, 'note'],
    ]);
    expect(fields[1].type).toBe(COLS_TYPE);
    expect(fields[7].type).toBe(BRK_TYPE);
    expect(markerFromRaw({ x: 1, o: 'text' }, 0)).toBeUndefined();
  });
  it('reads span and full row from the options', () => {
    expect(spanOf('number;rw;sp=2;mn=0')).toBe(2);
    expect(spanOf('number;rw')).toBeUndefined();
    expect(fullOf('switch;rw;fr=1')).toBe(true);
    expect(fullOf('switch;rw;from=x')).toBe(false);
  });
  it('picks grid columns by type, span and segment', () => {
    expect(gridColumnFor('switch')).toBe('auto');
    expect(gridColumnFor('text')).toBe('1 / -1');
    expect(gridColumnFor('number', undefined, true)).toBe('1 / -1');
    expect(gridColumnFor('text', 1)).toBe('auto');  // explicit span beats the type
    expect(gridColumnFor('number', 2, false, 3)).toEqual({ xs: '1 / -1', sm: 'span 2', md: 'span 2' });
    expect(gridColumnFor('number', 3, false, 2)).toEqual({ xs: '1 / -1', sm: 'span 2', md: 'span 2' });
  });
  it('shrinks columns on narrow screens', () => {
    expect(gridTemplateFor()).toBe('repeat(auto-fill, minmax(260px, 1fr))');
    expect(gridTemplateFor(3)).toEqual({ xs: '1fr', sm: 'repeat(2, 1fr)', md: 'repeat(3, 1fr)' });
    expect(gridTemplateFor(1)).toEqual({ xs: '1fr', sm: 'repeat(1, 1fr)', md: 'repeat(1, 1fr)' });
  });
  it('keeps 0 as a value', () => {
    expect(numberInputValue(0)).toBe(0);
    expect(numberInputValue(' 0')).toBe('0');  // older firmware padded "%2.0f"
    expect(numberInputValue('0.50')).toBe('0.50');
    expect(numberInputValue(null)).toBe('');
    expect(numberInputValue(undefined)).toBe('');
  });
});
