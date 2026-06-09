import { FC, useContext, useMemo } from 'react';
import { List } from '@mui/material';
import SettingsIcon from '@mui/icons-material/Settings';
import LockIcon from '@mui/icons-material/Lock';

import { AuthenticatedContext } from '../../contexts/authentication';
import { FeaturesContext } from '../../contexts/features';
import { useManifest } from '../../contexts/manifest';
import { resolveIcon } from '../dynamic-component/utils/iconRegistry';
import { FeatureEntry } from '../../types';
import LayoutMenuItem, { MenuNode } from './LayoutMenuItem';

// Manifest-driven side-nav — now a recursive TREE.
//
// A feature nests under another via `menu.parent` (the C++ backend is the
// single source of truth for placement: "drop X onto Y" == set X's
// menu.parent = "Y"); no parent = top-level. Arbitrary depth.
//
//  - A node WITH a route opens its page-with-tabs on click (as before).
//  - A route-less node is a pure GROUP whose row only toggles expand.
//  - `menu.expandTabs` additionally surfaces a feature's own tabs as
//    leaf children (navigate straight to /<id>/<tabKey>).
//
// Backward-compatible: with no `parent` declared anywhere this renders the
// exact flat list it did before. Order within a level is menu.order.
//
// Auth gating unchanged: an entry whose menu.auth === 'admin' renders
// disabled for non-admins; `menu.hidden` entries are skipped. The
// standalone Security page (not in the manifest) is appended as before.

const routePrefix = (entry: FeatureEntry): string | undefined =>
  entry.route ? entry.route.replace(/\/\*$/, '') : undefined;

const LayoutMenu: FC = () => {
  const { manifest } = useManifest();
  const { me } = useContext(AuthenticatedContext);
  const { features } = useContext(FeaturesContext);

  const roots = useMemo<MenuNode[]>(() => {
    const visible = manifest.features.filter((f) => f.menu?.label && !f.menu?.hidden);

    const nodeOf = (f: FeatureEntry): MenuNode => {
      const wantsAdmin = f.menu?.auth === 'admin' || f.auth === 'admin';
      const disabled = wantsAdmin && !me.admin;
      const to = routePrefix(f);
      const icon = resolveIcon(f.menu?.icon) ?? SettingsIcon;
      const children: MenuNode[] = [];
      // expandTabs: a multi-tab feature's own tabs become jump-to-tab
      // leaves under it (a single-tab feature's page IS the tab — no point).
      if (f.menu?.expandTabs && to && (f.tabs?.length ?? 0) > 1) {
        f.tabs!.forEach((t) =>
          children.push({
            id: `${f.id}/${t.key}`,
            label: t.title ?? t.key,
            // Per-tab icon if the backend declared one; else fall back to the
            // parent feature's icon so the leaf is never icon-less.
            icon: resolveIcon(t.icon) ?? icon,
            to: `${to}/${t.key}`,
            disabled,
            order: 0,
            isTab: true,   // marks an expandTabs leaf → grey background in the tree
            children: [],
          }),
        );
      }
      return {
        id: f.id,
        label: f.menu!.label!,
        icon,
        to,
        disabled,
        order: f.menu?.order ?? 0,
        children,
      };
    };

    const byId = new Map<string, MenuNode>();
    visible.forEach((f) => byId.set(f.id, nodeOf(f)));

    // Link children to parents; an orphan parent (id not registered)
    // gracefully falls back to top-level so nothing is ever lost.
    const rootNodes: MenuNode[] = [];
    visible.forEach((f) => {
      const node = byId.get(f.id)!;
      const parent = f.menu?.parent ? byId.get(f.menu.parent) : undefined;
      (parent ? parent.children : rootNodes).push(node);
    });

    const sortRec = (ns: MenuNode[]) => {
      ns.sort((a, b) => a.order - b.order);
      ns.forEach((n) => sortRec(n.children));
    };
    sortRec(rootNodes);
    return rootNodes;
  }, [manifest, me.admin]);

  return (
    <List disablePadding component="nav">
      {roots.map((n) => (
        <LayoutMenuItem key={n.id} node={n} />
      ))}
      {/* Standalone Security page (user CRUD) — not in the manifest; still
          appended explicitly, admin-gated, as before. */}
      {features.security && (
        <LayoutMenuItem
          node={{
            id: '__security',
            label: 'Security',
            icon: LockIcon,
            to: '/security',
            disabled: !me.admin,
            order: 9999,
            children: [],
          }}
        />
      )}
    </List>
  );
};

export default LayoutMenu;
