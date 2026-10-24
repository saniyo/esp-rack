// the UI's model of the external HW instances, built only from
// manifest.extHw (the backend HwRegistry is the one source). It says where an
// instance page goes: into an "External Hardware" group under its owner's
// menu entry, or under System when it has no owner (or the owner has no menu
// entry). The group label is the System tab's title, so the backend constant
// (HwRegistry::kLabel) names both.
import { ExtHwEntry, FeatureEntry, UiManifest } from '../../types/manifest';

export const SYSTEM_FEATURE_ID = 'system';
// Only when the System tab is compiled out (ESPRACK_HW_TAB=0).
export const EXT_HW_FALLBACK_LABEL = 'External Hardware';

export interface ExtHwModules {
  byId: Map<string, ExtHwEntry>;
  label: string;
}

export const extHwModules = (manifest: Pick<UiManifest, 'extHw' | 'features'>): ExtHwModules => {
  const byId = new Map<string, ExtHwEntry>();
  (manifest.extHw ?? []).forEach((e) => byId.set(e.id, e));
  const tab = manifest.features
    .find((f) => f.id === SYSTEM_FEATURE_ID)
    ?.tabs?.find((t) => t.key === 'hardware');
  return { byId, label: tab?.title || EXT_HW_FALLBACK_LABEL };
};

// The instance a page belongs to (its menu.hw), if the manifest lists it.
export const hwOf = (model: ExtHwModules, f: FeatureEntry): ExtHwEntry | undefined =>
  (f.menu?.hw ? model.byId.get(f.menu.hw) : undefined);

// The menu node that gets this page's "External Hardware" group: the owner,
// else System. undefined: not a hardware page. hasNode: a menu node exists.
export const hwGroupParent = (
  model: ExtHwModules,
  f: FeatureEntry,
  hasNode: (id: string) => boolean,
): string | undefined => {
  if (!f.menu?.hw) return undefined;
  const owner = hwOf(model, f)?.owner;
  return owner && hasNode(owner) ? owner : SYSTEM_FEATURE_ID;
};

// "Used by <this>": the owner's menu label, for a page of an owned instance.
export const usedByLabel = (
  model: ExtHwModules,
  f: FeatureEntry,
  features: FeatureEntry[],
): string | undefined => {
  const owner = hwOf(model, f)?.owner;
  if (!owner) return undefined;
  const of = features.find((x) => x.id === owner);
  return of?.menu?.label ?? of?.title ?? owner;
};
