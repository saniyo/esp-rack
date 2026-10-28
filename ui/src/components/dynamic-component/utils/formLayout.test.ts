// [host]: layout segments, grid columns, the 0 value.
import { describe, expect, it } from '@jest/globals';

import {
  BRK_TYPE, CELL_SX, COLS_TYPE, fullOf, gridColumnFor, gridTemplateFor, groupByCard, markerFromRaw, numberInputValue,
  numberStep, segmentFields, spanOf, visibleHeading,
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
  it('one column unless the page groups; elements keep their sizes', () => {
    expect(gridTemplateFor()).toBe('1fr');
    expect(gridTemplateFor(2)).toEqual({ xs: '1fr', sm: 'repeat(2, 1fr)', md: 'repeat(2, 1fr)' });
    expect(CELL_SX['& .MuiDivider-root']).toEqual({ display: 'none' });
    expect((CELL_SX as any)['& .MuiAvatar-root']).toBeUndefined();  // no shrunken icons
  });
  it('draws a heading once', () => {
    expect(visibleHeading('Status', ['Meter', 'Status'])).toBe('');
    expect(visibleHeading('  energy   METER ', ['Energy meter'])).toBe('');
    expect(visibleHeading('Energy meter and lamp', ['Meter', 'Status'])).toBe('Energy meter and lamp');
    expect(visibleHeading('', ['x'])).toBe('');
    expect(visibleHeading(undefined, [])).toBe('');
  });
});

describe('cards', () => {
  type E = { label: string; type: string; card?: string };
  const c = (label: string, card?: string): E => ({ label, type: 'number', card });
  const cols = (n: number, i: number) => markerFromRaw({ t: 'cols', n }, i)! as E;
  const brk = (i: number) => markerFromRaw({ t: 'brk' }, i)! as E;
  const group = (es: E[]) =>
    groupByCard(es, (e) => e, (e) => e.card).map((g) => ({ card: g.card, labels: g.entries.map((e) => e.label) }));

  it('gives a marker before a card to that card', () => {
    const es = [c('a', 'one'), c('b', 'one'), cols(3, 2), c('x', 'two'), c('y', 'two'), c('z', 'two')];
    expect(group(es)).toEqual([
      { card: 'one', labels: ['a', 'b'] },
      { card: 'two', labels: ['__cols2', 'x', 'y', 'z'] },
    ]);
    const two = groupByCard(es, (e) => e, (e) => e.card)[1];
    expect(segmentFields(two.entries, (e) => e)[0].cols).toBe(3);
  });
  it('lays out a card that opens the form', () => {
    const es = [cols(2, 0), c('a', 'one'), c('b', 'one')];
    expect(group(es)).toEqual([{ card: 'one', labels: ['__cols0', 'a', 'b'] }]);
  });
  it('keeps markers inside a card, and before bare fields with those', () => {
    const es = [c('a', 'one'), brk(1), c('b', 'one'), cols(2, 3), c('n'), c('m'), cols(2, 6), c('p', 'two')];
    expect(group(es)).toEqual([
      { card: 'one', labels: ['a', '__brk1', 'b'] },
      { card: undefined, labels: ['__cols3', 'n', 'm'] },
      { card: 'two', labels: ['__cols6', 'p'] },
    ]);
  });
  it('splits cards that are not consecutive, keeps trailing markers', () => {
    const es = [c('a', 'one'), c('n'), c('b', 'one'), brk(3)];
    expect(group(es)).toEqual([
      { card: 'one', labels: ['a'] },
      { card: undefined, labels: ['n'] },
      { card: 'one', labels: ['b', '__brk3'] },
    ]);
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
    expect(gridTemplateFor()).toBe('1fr');
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

describe('numberStep', () => {
  it('takes st=, else the format decimals', () => {
    expect(numberStep('number;rw;st=0.5;f=0.00')).toBe(0.5);
    expect(numberStep('number;rw;f=0.0')).toBe(0.1);
    expect(numberStep('number;rw;f=0.00')).toBe(0.01);
    expect(numberStep('number;rw;f=0')).toBe(1);
    expect(numberStep('number;rw;mn=0')).toBeUndefined();
    expect(numberStep(undefined)).toBeUndefined();
  });
});
