import { ComponentType, FC, useContext, useMemo, useState } from 'react';
import { useLocation } from 'react-router-dom';
import { List, SvgIconProps } from '@mui/material';
import SettingsIcon from '@mui/icons-material/Settings';
import LockIcon from '@mui/icons-material/Lock';
import FiberManualRecordIcon from '@mui/icons-material/FiberManualRecord';
import SpeedIcon from '@mui/icons-material/Speed';
import TuneIcon from '@mui/icons-material/Tune';
import SearchIcon from '@mui/icons-material/Search';
import NotificationsIcon from '@mui/icons-material/Notifications';
import HowToRegIcon from '@mui/icons-material/HowToReg';
import RestoreIcon from '@mui/icons-material/Restore';
import CodeIcon from '@mui/icons-material/Code';
import FingerprintIcon from '@mui/icons-material/Fingerprint';
import SystemUpdateAltIcon from '@mui/icons-material/SystemUpdateAlt';
import CloudUploadIcon from '@mui/icons-material/CloudUpload';
import UpdateIcon from '@mui/icons-material/Update';
import DevicesIcon from '@mui/icons-material/Devices';
import DnsIcon from '@mui/icons-material/Dns';
import SettingsApplicationsIcon from '@mui/icons-material/SettingsApplications';
import HttpIcon from '@mui/icons-material/Http';
import VpnKeyIcon from '@mui/icons-material/VpnKey';
import MemoryIcon from '@mui/icons-material/Memory';
import LanIcon from '@mui/icons-material/Lan';

import { AuthenticatedContext } from '../../contexts/authentication';
import { FeaturesContext } from '../../contexts/features';
import { useManifest } from '../../contexts/manifest';
import { resolveIcon } from '../dynamic-component/utils/iconRegistry';
import { FeatureEntry } from '../../types';
import { routeMatches } from '../../utils';
import LayoutMenuItem, { MenuNode } from './LayoutMenuItem';
import { Peer, SelfInfo, useDeviceMesh } from '../../utils/useDeviceMesh';

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

// Generic marker for an expandTabs leaf with no icon of its own: a small muted
// dot. Without this, tab leaves fell back to the PARENT feature's icon, so a
// feature's tabs all repeated the same glyph (e.g. three identical Wifi icons
// in a row), which looked odd. A neutral bullet reads cleanly as a sub-item.
const TabLeafDot: FC<SvgIconProps> = () => (
  <FiberManualRecordIcon sx={{ fontSize: 10, color: 'text.secondary' }} />
);

// Semantic fallback icons for expandTabs leaves, keyed by tab.key. A tab with
// no explicit backend icon gets a meaningful glyph here — and tabs that mean
// the SAME thing across features share ONE icon (every "status" looks the same,
// every "settings" the same, …). Explicit WebTab.icon() still wins; an unknown
// key falls through to the neutral dot.
const TAB_ICONS: Record<string, ComponentType<SvgIconProps>> = {
  status: SpeedIcon,                // live state / overview
  settings: TuneIcon,               // config (matches Light's Settings tab)
  scan: SearchIcon,                 // network scan
  subs: NotificationsIcon,          // subscriptions
  enrollment: HowToRegIcon,         // certificate enrolment
  recovery: RestoreIcon,            // certificate recovery
  internals: CodeIcon,              // raw internals / debug
  identity: FingerprintIcon,        // device identity
  ota: SystemUpdateAltIcon,         // OTA firmware update
  upload: CloudUploadIcon,          // firmware upload
  autoUpdate: UpdateIcon,           // scheduled auto-update
  clients: DevicesIcon,             // connected WebSocket clients
  mdns: DnsIcon,                    // mDNS service discovery
  configmgr: SettingsApplicationsIcon, // config manager
  endpoints: HttpIcon,              // HTTP endpoints
  jwt: VpnKeyIcon,                  // JWT secret / signing key
};

// The single root→leaf chain of node ids whose route is currently active.
// Seeds (and re-seeds on navigation) the accordion open path so the branch
// containing the current page is the one expanded. Children are checked before
// the node's own route so the deepest (most specific) match wins.
const activePath = (nodes: MenuNode[], pathname: string): string[] => {
  for (const n of nodes) {
    const sub = activePath(n.children, pathname);
    if (sub.length) return [n.id, ...sub];
    if (n.to && routeMatches(n.to, pathname)) return [n.id];
  }
  return [];
};

// Reshape the flat feature tree into the device-mesh layout:
//   <role feature(s)>   — e.g. Light, at the TOP; the "Device" group (all infra
//                         features: WiFi, NTP, … System) is nested INSIDE the
//                         role, so the role is this device's single home.
//   Discovery           — BELOW; its children are the device ROSTER: this
//                         device (a chip-icon entry) + the discovered peers
//                         (each a link, full page load, to its own web),
//                         sorted by canonical id (stable order everywhere).
// The device name is NEVER a top-level menu item — the top shows only the role
// + Discovery; the identity lives INSIDE Discovery.
// GRACEFUL: with no mesh module there is no "discovery" feature, so the flat
// tree is returned unchanged (classic single-device menu).
const buildDeviceTree = (
  localRoots: MenuNode[],
  self: SelfInfo | null,
  peers: Peer[],
): MenuNode[] => {
  const discoveryIdx = localRoots.findIndex((n) => n.id === 'discovery');
  if (discoveryIdx === -1) return localRoots;

  const discovery = localRoots[discoveryIdx];
  const rest = localRoots.filter((_, i) => i !== discoveryIdx);
  const roleNodes = rest.filter((n) => n.isRole);
  const infraNodes = rest.filter((n) => !n.isRole);

  // Everything that isn't the role collapses into one "Device" group.
  const deviceGroup: MenuNode = {
    id: '__device',
    label: 'Device',
    icon: SettingsIcon,
    order: 500,
    children: infraNodes,
  };

  // Discovery roster: this device + the discovered peers, sorted by canonical
  // id (stable everywhere).
  const roster = [
    ...(self
      ? [{ id: self.device_id, project: self.project, ip: self.ip, isLocal: true, compatible: true }]
      : []),
    ...peers.map((p) => ({
      id: p.device_id, project: p.project, ip: p.ip, isLocal: false, compatible: p.compatible,
    })),
  ].sort((a, b) => a.id.localeCompare(b.id));

  const rosterNodes: MenuNode[] = roster.map((e) =>
    e.isLocal
      ? // this device — shown with its ROLE icon (e.g. Light's Lightbulb),
        // not a generic chip; falls back to the chip if there's no role.
        { id: '__self', label: e.project || 'Device', icon: roleNodes[0]?.icon ?? MemoryIcon, order: 0, children: [] }
      : {
          id: `__peer_${e.id}`,
          label: e.project || 'Device',
          icon: LanIcon,
          to: `http://${e.ip}/`,
          external: true,        // navigate (full page load) to the peer's web
          disabled: !e.compatible,
          order: 0,
          children: [] as MenuNode[],
        },
  );
  const discoveryNode: MenuNode = { ...discovery, order: 9000, children: rosterNodes };

  // Nest the "Device" group UNDER the role feature: the role is this device's
  // home — its own tabs + the Device group inside it. (No role feature -> the
  // Device group stays top-level.) Top level then = [role, Discovery].
  const [firstRole, ...otherRoles] = roleNodes;
  const topNodes: MenuNode[] = firstRole
    ? [{ ...firstRole, children: [...firstRole.children, deviceGroup] }, ...otherRoles]
    : [deviceGroup];

  return [...topNodes, discoveryNode];
};

const LayoutMenu: FC = () => {
  const { manifest } = useManifest();
  const { me } = useContext(AuthenticatedContext);
  const { features } = useContext(FeaturesContext);

  const localRoots = useMemo<MenuNode[]>(() => {
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
            // Icon priority: explicit backend WebTab.icon() → semantic icon by
            // tab.key (so every "status"/"settings"/… shares one glyph) → a
            // neutral dot for anything unmapped. Never the parent's icon (which
            // made a feature's tabs all repeat the same glyph, looking odd).
            icon: resolveIcon(t.icon) ?? TAB_ICONS[t.key] ?? TabLeafDot,
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
        isRole: f.menu?.role,   // device's main-role feature (-> direct child)
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
    // Standalone Security page (not a manifest feature) — fold it in as a
    // top-level infra item so it lands inside the "Device" group like the
    // other framework features.
    if (features.security) {
      rootNodes.push({
        id: '__security', label: 'Security', icon: LockIcon,
        to: '/security', disabled: !me.admin, order: 9999, children: [],
      });
    }
    sortRec(rootNodes);
    return rootNodes;
  }, [manifest, me.admin, features.security]);

  // Discovered LAN peers (empty/graceful when the mesh module isn't installed).
  const { self, peers } = useDeviceMesh();
  // Device-mesh layout: role + "Device" infra group at the top, then a
  // "Discovery" group whose roster is this device + the discovered peers.
  const roots = useMemo<MenuNode[]>(
    () => buildDeviceTree(localRoots, self, peers),
    [localRoots, self, peers],
  );

  // ── Accordion open-state: ONE shared path for the whole tree (see
  // LayoutMenuItem). Opening any branch collapses the previously open one. ──
  // Seeded ONCE, on mount, to the branch of the page you land on (deep-link /
  // refresh orientation). After that the tree is driven purely by the chevrons:
  // navigating by clicking a row does NOT auto-expand — the menu shows only what
  // the user explicitly opened, so a click never "expands everything".
  const { pathname } = useLocation();
  const [openPath, setOpenPath] = useState<string[]>(() => activePath(roots, pathname));

  return (
    <List disablePadding component="nav">
      {roots.map((n) => (
        <LayoutMenuItem key={n.id} node={n} openPath={openPath} setOpenPath={setOpenPath} />
      ))}
    </List>
  );
};

export default LayoutMenu;
