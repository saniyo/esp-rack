// SES-LIB-030 [host]: compact (schemaVersion 4) entries normalise back to the
// full ones; compact() mirrors the firmware's compactManifestEntry().
import { describe, expect, it } from '@jest/globals';

import { findAction } from '../../utils/manifestEndpoints';
import { mergeActions, normalizeEntry, normalizeManifest, ownerOfAction } from './normalizeManifest';

const API = '/api/v1';
export function compact(f: any): any {
  const o = JSON.parse(JSON.stringify(f));
  const dropIf = (x: any, k: string, v: any) => { if (x[k] === v) delete x[k]; };
  const act = (a: any) => {
    dropIf(a, 'restPath', `${API}/actions/${a.id}`);
    dropIf(a, 'method', 'POST');
    dropIf(a, 'auth', 'authenticated');
  };
  const kind = o.kind ?? 'feature';
  if (kind === 'action') { act(o); return o; }
  const auth = o.auth ?? 'authenticated';
  const form = `${API}/${o.id}/form`;
  const state = `${API}/${o.id}/state`;
  dropIf(o, 'kind', 'feature');
  dropIf(o, 'component', kind === 'page' ? '' : 'DynamicSettings');
  dropIf(o, 'route', `/${o.id}/*`);
  if (o.menu) {
    dropIf(o.menu, 'label', o.title);
    if (!o.menu.order) delete o.menu.order;
    dropIf(o.menu, 'auth', auth);
    if (o.menu.hidden === false) delete o.menu.hidden;
    if (o.menu.expandTabs) delete o.menu.expandTabs; else o.menu.expandTabs = false;
  }
  if (o.rest && o.rest.read === form && o.rest.update === state && o.rest.state === state
      && Object.keys(o.rest).length === 3) o.rest = 1;
  (o.tabs ?? []).forEach((t: any) => {
    dropIf(t, 'restPath', form);
    if (t.postable === false) delete t.postable;
    if (t.live === false) delete t.live;
    dropIf(t, 'auth', auth);
  });
  (o.actions ?? []).forEach(act);
  (o.endpoints ?? []).forEach((e: any) => dropIf(e, 'auth', 'authenticated'));
  for (const k of ['tabs', 'actions', 'endpoints']) if (Array.isArray(o[k]) && o[k].length === 0) delete o[k];
  dropIf(o, 'auth', 'authenticated');
  return o;
}

const feature = (id: string, extra: any = {}) => ({
  id, kind: 'feature', title: `T ${id}`, component: 'DynamicSettings', auth: 'authenticated', route: `/${id}/*`,
  menu: { label: `T ${id}`, icon: 'Wifi', order: 0, auth: 'authenticated', hidden: false, expandTabs: true },
  rest: { read: `${API}/${id}/form`, update: `${API}/${id}/state`, state: `${API}/${id}/state` },
  ch: id,
  tabs: [
    { key: 'status', title: 'Status', restPath: `${API}/${id}/form`, postable: false, live: true, auth: 'authenticated' },
    { key: 'settings', title: 'Settings', restPath: `${API}/${id}/form`, postable: true, live: false, auth: 'admin' },
  ],
  actions: [{ id: `${id}.go`, title: 'Go', restPath: `${API}/actions/${id}.go`, method: 'POST', auth: 'admin' }],
  ...extra,
});
const page = { id: 'fs', kind: 'page', title: 'Files', component: '', auth: 'admin', route: '/fs/*',
  endpoints: [{ method: 'GET', path: '/api/fs', auth: 'authenticated' }], actions: [] };
const action = { id: 'system.restart', kind: 'action', title: 'Restart', restPath: `${API}/actions/system.restart`,
  method: 'POST', auth: 'admin', confirm: 'Sure?' };

describe('compact manifest', () => {
  it('normalises compact entries back to the full ones', () => {
    for (const f of [feature('wifi'), feature('hw1', { menu: { label: 'Relay', order: 61, auth: 'admin',
      hidden: false, expandTabs: false, hw: 'hw1' } }), page, action]) {
      expect(normalizeEntry(compact(f))).toEqual(f);
    }
  });
  it('leaves a schemaVersion 3 manifest alone', () => {
    const m = { schemaVersion: 3, features: [{ id: 'x', title: 'X' } as any] };
    expect(normalizeManifest(m as any)).toBe(m);
  });
  it('grows linearly and stays small per entry', () => {
    const n = 60;
    const full = Array.from({ length: n }, (_, i) => feature(`feat${i}`));
    const size = (k: number) => JSON.stringify(full.slice(0, k).map(compact)).length;
    const per = (size(n) - size(n / 2)) / (n / 2);
    expect(Math.abs(per - size(n / 2) / (n / 2))).toBeLessThan(4);  // linear
    expect(per).toBeLessThan(0.6 * (JSON.stringify(full).length / n));  // well under the full entry
    const m = normalizeManifest({ schemaVersion: 4, features: full.map(compact) } as any);
    expect(m.features).toEqual(full);
  });
});

describe('lazy feature actions', () => {
  it('finds a fetched action like one from the manifest', () => {
    const m = normalizeManifest({ schemaVersion: 4, features: [compact(feature('meter')), compact(action)] } as any);
    expect(findAction(m, 'meter.reset')).toBeFalsy();
    const fetched = [{ id: 'meter.reset', kind: 'action', title: 'Reset', restPath: `${API}/actions/meter.reset`,
      method: 'POST', auth: 'admin' }];
    const m2 = mergeActions(m, fetched as any);
    expect(findAction(m2, 'meter.reset')).toBeTruthy();
    expect(findAction(m2, 'system.restart')).toBeTruthy();  // global actions stay
    expect(mergeActions(m2, fetched as any)).toBe(m2);  // no duplicates
    expect(ownerOfAction('meter.reset')).toBe('meter');
    expect(ownerOfAction('plain')).toBe('');
  });
});
