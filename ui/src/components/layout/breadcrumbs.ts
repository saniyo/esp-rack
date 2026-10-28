// The path to a page through the menu tree (menu.parent), for the
// breadcrumbs in the app bar: root first, the page itself last.
import { FeatureEntry } from '../../types';

export interface Crumb {
  id: string;
  label: string;
  to?: string;  // its page; undefined: a group without one
}

const MAX_DEPTH = 8;

export const crumbLabel = (f: FeatureEntry): string => f.menu?.label ?? f.title ?? f.id;
export const crumbRoute = (f: FeatureEntry): string | undefined =>
  f.route ? f.route.replace(/\/\*$/, '') : undefined;

export function featureChain(features: FeatureEntry[], id: string): Crumb[] {
  const byId = new Map(features.map((f) => [f.id, f]));
  const out: Crumb[] = [];
  const seen = new Set<string>();
  let f = byId.get(id);
  while (f && !seen.has(f.id) && out.length < MAX_DEPTH) {
    seen.add(f.id);
    out.unshift({ id: f.id, label: crumbLabel(f), to: crumbRoute(f) });
    f = f.menu?.parent ? byId.get(f.menu.parent) : undefined;
  }
  return out;
}

// The page a path belongs to: the feature whose route is the longest prefix.
export function featureOfPath(features: FeatureEntry[], pathname: string): FeatureEntry | undefined {
  let best: FeatureEntry | undefined;
  let bestLen = 0;
  features.forEach((f) => {
    const r = crumbRoute(f);
    if (r && r.length > bestLen && (pathname === r || pathname.startsWith(`${r}/`))) {
      best = f;
      bestLen = r.length;
    }
  });
  return best;
}
