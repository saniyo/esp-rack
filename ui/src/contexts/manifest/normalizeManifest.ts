// SES-LIB-030: the cached manifest (schemaVersion >= 4) leaves out what follows
// from an entry's id or is the default. normalizeManifest() puts it back once,
// at load, so the rest of the UI reads full entries. A schemaVersion 3
// manifest (older firmware) passes through unchanged.
import { FeatureEntry, UiManifest } from '../../types/manifest';

const API = '/api/v1';

const action = (a: any) => ({
  ...a,
  restPath: a.restPath ?? `${API}/actions/${a.id}`,
  method: a.method ?? 'POST',
  auth: a.auth ?? 'authenticated',
});

export function normalizeEntry(f: any): FeatureEntry {
  const kind = f.kind ?? 'feature';
  if (kind === 'action') return action({ ...f, kind }) as FeatureEntry;
  const id = f.id ?? '';
  const auth = f.auth ?? 'authenticated';
  const form = `${API}/${id}/form`;
  const state = `${API}/${id}/state`;
  const out: any = {
    ...f,
    kind,
    auth,
    component: f.component ?? (kind === 'page' ? '' : 'DynamicSettings'),
    route: 'route' in f ? f.route : `/${id}/*`,
    actions: (f.actions ?? []).map(action),
  };
  if (kind === 'feature') {
    out.tabs = (f.tabs ?? []).map((t: any) => ({
      ...t,
      restPath: t.restPath ?? form,
      postable: t.postable ?? false,
      live: t.live ?? false,
      auth: t.auth ?? auth,
    }));
  }
  if (kind === 'page') out.endpoints = (f.endpoints ?? []).map((e: any) => ({ ...e, auth: e.auth ?? 'authenticated' }));
  if (f.rest === 1) out.rest = { read: form, update: state, state };
  if (f.menu) {
    out.menu = {
      ...f.menu,
      label: f.menu.label ?? f.title,
      order: f.menu.order ?? 0,
      auth: f.menu.auth ?? auth,
      hidden: f.menu.hidden ?? false,
      expandTabs: f.menu.expandTabs ?? true,
    };
  }
  return out as FeatureEntry;
}

export function normalizeManifest(m: UiManifest): UiManifest {
  if (!m || (m.schemaVersion ?? 0) < 4) return m;
  return { ...m, features: (m.features ?? []).map(normalizeEntry) };
}

// SES-LIB-030: a feature's own actions, fetched when needed, join the manifest
// list, so every lookup finds them like any other entry.
export function mergeActions(m: UiManifest, actions: FeatureEntry[]): UiManifest {
  const have = new Set(m.features.map((f) => f.id));
  const add = actions.map(normalizeEntry).filter((a) => !have.has(a.id));
  return add.length ? { ...m, features: [...m.features, ...add] } : m;
}

// "meter.reset" -> "meter": the feature an action belongs to by its id.
export const ownerOfAction = (id: string): string => (id.indexOf('.') > 0 ? id.slice(0, id.indexOf('.')) : '');
