import React, { FC, useState, useEffect } from "react";
import { Link, useLocation } from "react-router-dom";
import {
  ListItem,
  ListItemButton,
  ListItemIcon,
  ListItemText,
  SvgIconProps,
  IconButton,
  Collapse,
  List,
} from "@mui/material";
import ExpandMore from "@mui/icons-material/ExpandMore";
import ExpandLess from "@mui/icons-material/ExpandLess";

import { routeMatches } from "../../utils";

// One node of the recursive sidebar tree.
//  - `to` set       => navigable: clicking the row opens its page-with-tabs.
//  - `to` undefined => pure group: the row only toggles expand/collapse.
//  - children       => child features and/or this feature's own tabs.
// A node with children gets a chevron button on the RIGHT (a separate
// click target) so the row body still navigates while the chevron expands.
export interface MenuNode {
  id: string;
  label: string;
  icon: React.ComponentType<SvgIconProps>;
  to?: string;
  disabled?: boolean;
  order: number;
  // True for an expandTabs leaf (a component's own internal tab). Rendered on
  // a grey background to distinguish from nested feature components (white).
  isTab?: boolean;
  children: MenuNode[];
}

// Is the current route this node or anywhere in its subtree? Drives
// auto-expanding the path down to the active page.
const subtreeActive = (node: MenuNode, pathname: string): boolean =>
  (!!node.to && routeMatches(node.to, pathname)) ||
  node.children.some((c) => subtreeActive(c, pathname));

interface Props {
  node: MenuNode;
  depth?: number;
  // Ancestor ids on the path to this node. A plain string[] (not Set) so
  // the spread below downlevels cleanly without --downlevelIteration.
  ancestors?: string[];
}

const LayoutMenuItem: FC<Props> = ({ node, depth = 0, ancestors = [] }) => {
  const { pathname } = useLocation();

  // Cycle guard: never recurse into a child that is already one of our
  // ancestors (a misconfigured menu.parent loop would recurse forever).
  const kids = node.children.filter((c) => ancestors.indexOf(c.id) === -1);
  const hasKids = kids.length > 0;

  const navigable = !!node.to;
  // Selected = this node's route matches AND no child's route matches more
  // deeply — so the active leaf (the open sub-tab) takes the highlight, not
  // its parent. A feature with no matching child still highlights on its own
  // sub-routes (e.g. WiFi on /wifi/status, which has no menu leaf).
  const active =
    navigable &&
    routeMatches(node.to!, pathname) &&
    !node.children.some((c) => !!c.to && routeMatches(c.to!, pathname));

  const [open, setOpen] = useState<boolean>(() => subtreeActive(node, pathname));
  useEffect(() => {
    if (subtreeActive(node, pathname)) setOpen(true); // open down to active page
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [pathname]);

  const toggle = (e?: React.MouseEvent) => {
    e?.preventDefault();
    e?.stopPropagation();
    setOpen((o) => !o);
  };

  const Icon = node.icon;
  const childAnc = [...ancestors, node.id];
  // Sub-tab leaves (expandTabs) get a subtle grey background so they read as
  // the component's own internal tabs, distinct from nested feature components
  // (which stay on the default white). Dropped when active, so the normal
  // `selected` highlight shows which tab is currently open.
  const sx = {
    pl: 2 + depth * 2,
    pr: hasKids ? 6 : 2,
    // Sub-tab leaves form a cohesive GREY panel under the component (always
    // grey, including the active one), so they clearly read as the
    // component's own internal tabs vs. nested feature components (white).
    // The active tab keeps the selected tint layered on top of the grey.
    ...(node.isTab
      ? {
          bgcolor: "grey.200",
          "&:hover": { bgcolor: "grey.300" },
          "&.Mui-selected": { bgcolor: "action.selected" },
          "&.Mui-selected:hover": { bgcolor: "action.selected" },
        }
      : {}),
  };

  return (
    <>
      <ListItem
        disablePadding
        secondaryAction={
          hasKids ? (
            <IconButton
              edge="end"
              size="small"
              onClick={toggle}
              aria-label={open ? "collapse" : "expand"}
            >
              {open ? <ExpandLess /> : <ExpandMore />}
            </IconButton>
          ) : undefined
        }
      >
        {navigable ? (
          <ListItemButton component={Link} to={node.to!} selected={active} disabled={node.disabled} sx={sx}>
            <ListItemIcon sx={{ minWidth: 36 }}>
              <Icon />
            </ListItemIcon>
            <ListItemText>{node.label}</ListItemText>
          </ListItemButton>
        ) : (
          <ListItemButton onClick={() => toggle()} selected={active} disabled={node.disabled} sx={sx}>
            <ListItemIcon sx={{ minWidth: 36 }}>
              <Icon />
            </ListItemIcon>
            <ListItemText>{node.label}</ListItemText>
          </ListItemButton>
        )}
      </ListItem>
      {hasKids && (
        <Collapse in={open} timeout="auto" unmountOnExit>
          <List disablePadding component="div">
            {kids.map((c) => (
              <LayoutMenuItem key={c.id} node={c} depth={depth + 1} ancestors={childAnc} />
            ))}
          </List>
        </Collapse>
      )}
    </>
  );
};

export default LayoutMenuItem;
