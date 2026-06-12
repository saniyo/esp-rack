import React, { FC } from "react";
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
import OpenInNewIcon from "@mui/icons-material/OpenInNew";

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
  // a grey panel to distinguish it from nested feature components (white).
  isTab?: boolean;
  // True for the device's MAIN-ROLE feature — renders directly under the
  // device record in the mesh menu (vs. the "Device" infra group).
  isRole?: boolean;
  // True for a REMOTE device record: `to` is a full URL (http://peer-ip/) and
  // clicking navigates the browser there (full page load), not an SPA route —
  // each device serves its own web; the menu just links between them.
  external?: boolean;
  children: MenuNode[];
}

interface Props {
  node: MenuNode;
  depth?: number;
  // Ancestor ids on the path to this node. A plain string[] (not Set) so the
  // spread below downlevels cleanly without --downlevelIteration.
  ancestors?: string[];
  // ACCORDION: the whole tree shares ONE "open path" — the single chain of ids
  // from a root down to the deepest open node. A node is open iff its id is on
  // that path; opening a node sets the path to [...itsAncestors, itsId], which
  // collapses every sibling branch at every level. Owned by LayoutMenu so that
  // expanding one group auto-closes the previously open one (less clutter).
  openPath: string[];
  setOpenPath: (path: string[]) => void;
}

const LayoutMenuItem: FC<Props> = ({
  node,
  depth = 0,
  ancestors = [],
  openPath,
  setOpenPath,
}) => {
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
    !node.external &&
    routeMatches(node.to!, pathname) &&
    !node.children.some((c) => !!c.to && routeMatches(c.to!, pathname));

  // Open iff this node sits on the shared open path.
  const open = openPath.indexOf(node.id) !== -1;

  const toggle = (e?: React.MouseEvent) => {
    e?.preventDefault();
    e?.stopPropagation();
    // Accordion: open  => set the path to me (collapses sibling branches);
    //            close => fall back to my ancestors (drop me + everything below).
    setOpenPath(open ? ancestors : [...ancestors, node.id]);
  };

  const Icon = node.icon;
  const childAnc = [...ancestors, node.id];

  // Sub-tab leaves (expandTabs) sit on a clearly grey panel so they read as the
  // component's own internal tabs; nested feature components stay on white.
  // The ACTIVE tab fills the WHOLE row with a noticeably DARKER neutral grey +
  // bold (700) label — a big lightness step (grey.300 → grey.500) so the open
  // tab is unmistakable. background-color eases smoothly on hover/selection.
  const sx = {
    pl: 2 + depth * 2,
    pr: hasKids || node.external ? 6 : 2,
    transition: "background-color 0.2s ease",
    ...(node.isTab
      ? {
          bgcolor: "grey.300",
          "&:hover": { bgcolor: "grey.400" },
          "&.Mui-selected": {
            bgcolor: "grey.500",
            "& .MuiListItemText-primary": { fontWeight: 700 },
          },
          "&.Mui-selected:hover": { bgcolor: "grey.500" },
        }
      : {}),
  };

  return (
    <>
      <ListItem
        disablePadding
        secondaryAction={
          node.external ? (
            <OpenInNewIcon
              fontSize="small"
              sx={{ color: "text.disabled", pointerEvents: "none", mr: 0.5 }}
            />
          ) : hasKids ? (
            <IconButton
              edge="end"
              size="small"
              onClick={toggle}
              aria-label={open ? "collapse" : "expand"}
            >
              <ExpandMore
                sx={{
                  transition: "transform 0.2s ease",
                  transform: open ? "rotate(180deg)" : "rotate(0deg)",
                }}
              />
            </IconButton>
          ) : undefined
        }
      >
        {node.external ? (
          // Remote device: a native anchor — full page load to the peer's web.
          <ListItemButton component="a" href={node.to!} disabled={node.disabled} sx={sx}>
            <ListItemIcon sx={{ minWidth: 36 }}>
              <Icon />
            </ListItemIcon>
            <ListItemText>{node.label}</ListItemText>
          </ListItemButton>
        ) : navigable ? (
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
              <LayoutMenuItem
                key={c.id}
                node={c}
                depth={depth + 1}
                ancestors={childAnc}
                openPath={openPath}
                setOpenPath={setOpenPath}
              />
            ))}
          </List>
        </Collapse>
      )}
    </>
  );
};

export default LayoutMenuItem;
