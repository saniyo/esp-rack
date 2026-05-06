import { FC, useContext, useMemo } from 'react';
import { List } from '@mui/material';
import SettingsIcon from '@mui/icons-material/Settings';
import LockIcon from '@mui/icons-material/Lock';

import { AuthenticatedContext } from '../../contexts/authentication';
import { FeaturesContext } from '../../contexts/features';
import { useManifest } from '../../contexts/manifest';
import { resolveIcon } from '../dynamic-component/utils/iconRegistry';
import { FeatureEntry } from '../../types';
import LayoutMenuItem from './LayoutMenuItem';

// Manifest-driven side-nav. Every feature whose manifest entry has a
// menu.label and a route is surfaced here automatically — adding a new
// C++ service that registers a feature with menu metadata makes its tab
// appear without touching the frontend. Order is honored from
// menu.order (smaller = higher); ties keep manifest insertion order.
//
// Auth gating: an entry whose menu.auth === 'admin' renders disabled
// for non-admin users (matches the legacy hardcoded /security and
// /filesystem treatment). Entries flagged `menu.hidden` are skipped.
//
// Standalone Security page (user CRUD) is not in the manifest today;
// its menu item is rendered alongside the manifest-driven list, also
// admin-gated. Anyone wanting to drop it can disable FT_SECURITY in
// the build, which removes the AuthenticatedRouting route + this menu
// surfaces nothing for it (the FeaturesContext gate suppresses it).

const itemPathOf = (entry: FeatureEntry): string => {
  // route is "/<id>/*" or "/<custom>/*". Menu link wants the prefix
  // without the wildcard suffix.
  const r = entry.route ?? `/${entry.id}`;
  return r.replace(/\/\*$/, '');
};

const LayoutMenu: FC = () => {
  const { manifest } = useManifest();
  const { me } = useContext(AuthenticatedContext);
  const { features } = useContext(FeaturesContext);

  const items = useMemo(() => {
    return manifest.features
      .filter((f) => f.menu?.label && !f.menu?.hidden)
      .slice()
      .sort((a, b) => (a.menu?.order ?? 0) - (b.menu?.order ?? 0));
  }, [manifest]);

  return (
    <List disablePadding component="nav">
      {items.map((f) => {
        const Icon = resolveIcon(f.menu?.icon) ?? SettingsIcon;
        const wantsAdmin = f.menu?.auth === 'admin' || f.auth === 'admin';
        const disabled = wantsAdmin && !me.admin;
        return (
          <LayoutMenuItem
            key={f.id}
            icon={Icon}
            label={f.menu!.label!}
            to={itemPathOf(f)}
            disabled={disabled}
          />
        );
      })}
      {/* Standalone Security page (user CRUD) — not in manifest yet,
          rendered explicitly. When SecurityModule grows a top-level
          manifest entry for the user list this block can be deleted. */}
      {features.security && (
        <LayoutMenuItem
          icon={LockIcon}
          label="Security"
          to="/security"
          disabled={!me.admin}
        />
      )}
    </List>
  );
};

export default LayoutMenu;
