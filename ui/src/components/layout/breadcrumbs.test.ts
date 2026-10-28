import { describe, expect, it } from '@jest/globals';

import { FeatureEntry } from '../../types';
import { featureChain, featureOfPath } from './breadcrumbs';

const f = (id: string, label: string, parent?: string): FeatureEntry => ({
  id, kind: 'feature', route: `/${id}/*`, menu: { label, parent },
});
const features = [
  f('gateway', 'Inverter Gateway'),
  f('jk', 'Balancers', 'gateway'),
  f('jkBus', 'JK-DZ08 CAN bus', 'jk'),
  f('can0', 'can0', 'jkBus'),
  f('loopA', 'A', 'loopB'),
  f('loopB', 'B', 'loopA'),
];

describe('breadcrumbs', () => {
  it('walks the menu tree to the root', () => {
    expect(featureChain(features, 'can0').map((c) => c.label)).toEqual([
      'Inverter Gateway', 'Balancers', 'JK-DZ08 CAN bus', 'can0',
    ]);
    expect(featureChain(features, 'can0')[1].to).toBe('/jk');
  });
  it('has one crumb for a top-level page and stops at a loop', () => {
    expect(featureChain(features, 'gateway')).toHaveLength(1);
    expect(featureChain(features, 'loopA')).toHaveLength(2);
  });
  it('finds the page of a path', () => {
    expect(featureOfPath(features, '/jkBus/status')?.id).toBe('jkBus');
    expect(featureOfPath(features, '/nowhere')).toBeUndefined();
  });
});
