// [host]: where external HW pages go, from manifest.extHw only.
import { describe, expect, it } from '@jest/globals';

import { ExtHwEntry, FeatureEntry } from '../../types/manifest';
import { EXT_HW_FALLBACK_LABEL, extHwModules, hwGroupParent, usedByLabel } from './extHwMenu';

const feat = (id: string, menu: FeatureEntry['menu'], tabs?: FeatureEntry['tabs']): FeatureEntry => ({
  id, kind: 'feature', menu, tabs,
});

const owner = feat('controller', { label: 'Controller' });
const system = feat('system', { label: 'System' }, [{ key: 'hardware', title: 'External Hardware', restPath: '/x' }]);
const meter = feat('meter', { label: 'Meter', hw: 'meter' });
const relay = feat('relay', { label: 'Relay', hw: 'relay' });
const orphan = feat('orphan', { label: 'Orphan', hw: 'orphan' });
const plain = feat('plain', { label: 'Plain', parent: 'controller' });
const features = [owner, system, meter, relay, orphan, plain];
const extHw: ExtHwEntry[] = [
  { id: 'meter', type: 'energy-meter', owner: 'controller', pages: ['meter'] },
  { id: 'relay', type: 'digital-io', pages: ['relay'] },
  { id: 'orphan', type: 'x', owner: 'noMenu', pages: ['orphan'] },
];
const has = (id: string) => features.some((f) => f.id === id);

describe('extHw placement', () => {
  const m = extHwModules({ features, extHw });

  it('puts an owned page under its owner', () => {
    expect(hwGroupParent(m, meter, has)).toBe('controller');
  });
  it('puts an unowned page under System', () => {
    expect(hwGroupParent(m, relay, has)).toBe('system');
  });
  it('falls back to System when the owner has no menu entry', () => {
    expect(hwGroupParent(m, orphan, has)).toBe('system');
  });
  it('leaves other pages alone', () => {
    expect(hwGroupParent(m, plain, has)).toBeUndefined();
  });
  it('takes the label from the System tab', () => {
    expect(m.label).toBe('External Hardware');
    expect(extHwModules({ features: [owner], extHw: [] }).label).toBe(EXT_HW_FALLBACK_LABEL);
  });
  it('names the owner for "Used by"', () => {
    expect(usedByLabel(m, meter, features)).toBe('Controller');
    expect(usedByLabel(m, relay, features)).toBeUndefined();
  });
});
