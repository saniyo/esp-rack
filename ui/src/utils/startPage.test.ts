import { describe, expect, it } from '@jest/globals';

import { FeatureEntry } from '../types';
import { startPath } from './startPage';

const f = (id: string, menu: Record<string, unknown> = {}): FeatureEntry =>
  ({ id, kind: 'feature', route: `/${id}/*`, menu: { label: id, ...menu } } as FeatureEntry);

describe('start page', () => {
  it('opens the first menu item when nothing is marked (as before)', () => {
    expect(startPath([f('wifi'), f('system')])).toBe('/wifi');
  });
  it('prefers the role feature', () => {
    expect(startPath([f('wifi'), f('gateway', { role: true })])).toBe('/gateway');
  });
  it('prefers a landing page over the role feature', () => {
    expect(startPath([f('wifi'), f('gateway', { role: true }), f('overview', { landing: true })])).toBe('/overview');
  });
  it('skips hidden items and falls back to /system', () => {
    expect(startPath([f('x', { hidden: true, landing: true })])).toBe('/system');
  });
});
