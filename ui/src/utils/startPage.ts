// The page the UI opens first (and the brand in the sidebar leads to): the
// feature marked landing, else the device's role feature (the application),
// else the first menu item, as before.
import { FeatureEntry } from '../types';

const routeOf = (f?: FeatureEntry) => (f?.route ? f.route.replace(/\/\*$/, '') : undefined);

export function startPath(features: FeatureEntry[]): string {
  const visible = features.filter((f) => f.menu?.label && !f.menu?.hidden && f.route);
  const pick =
    visible.find((f) => f.menu?.landing) ?? visible.find((f) => f.menu?.role) ?? visible[0];
  return routeOf(pick) ?? '/system';
}
