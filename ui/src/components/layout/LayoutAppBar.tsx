
import { useContext, useMemo, FC } from 'react';
import { Link as RouterLink, useLocation } from 'react-router-dom';
import { AppBar, Box, Breadcrumbs, IconButton, Link, Toolbar, Typography } from '@mui/material';
import MenuIcon from '@mui/icons-material/Menu';

import { FeaturesContext } from '../../contexts/features';
import { useManifest } from '../../contexts/manifest/context';
import { featureChain, featureOfPath } from './breadcrumbs';

import LayoutAuthMenu from './LayoutAuthMenu';

export const DRAWER_WIDTH = 280;

interface LayoutAppBarProps {
  title: string;
  onToggleDrawer: () => void;
}

const LayoutAppBar: FC<LayoutAppBarProps> = ({ title, onToggleDrawer }) => {
  const { features } = useContext(FeaturesContext);
  const { manifest } = useManifest();
  const { pathname } = useLocation();
  // A page inside the menu tree shows its path (root › … › page); a
  // top-level page shows its title as before.
  const chain = useMemo(() => {
    const page = featureOfPath(manifest.features, pathname);
    return page ? featureChain(manifest.features, page.id) : [];
  }, [manifest.features, pathname]);

  return (
    <AppBar
      position="fixed"
      sx={{
        width: { md: `calc(100% - ${DRAWER_WIDTH}px)` },
        ml: { md: `${DRAWER_WIDTH}px` },
        boxShadow: 'none'
      }}
    >
      <Toolbar>
        <IconButton
          color="inherit"
          aria-label="open drawer"
          edge="start"
          onClick={onToggleDrawer}
          sx={{ mr: 2, display: { md: 'none' } }}
        >
          <MenuIcon />
        </IconButton>
        {/* A narrow screen keeps the title; wider ones show the path to it. */}
        <Typography variant="h6" noWrap component="div" sx={{ display: { xs: 'block', sm: chain.length > 1 ? 'none' : 'block' } }}>
          {title}
        </Typography>
        {chain.length > 1 && (
          <Breadcrumbs
            color="inherit"
            maxItems={4}
            itemsBeforeCollapse={1}
            itemsAfterCollapse={2}
            sx={{ display: { xs: 'none', sm: 'flex' }, minWidth: 0, '& .MuiBreadcrumbs-ol': { flexWrap: 'nowrap' } }}
          >
            {chain.slice(0, -1).map((c) =>
              c.to ? (
                <Link key={c.id} component={RouterLink} to={c.to} color="inherit" underline="hover" noWrap sx={{ opacity: 0.85 }}>
                  {c.label}
                </Link>
              ) : (
                <Typography key={c.id} color="inherit" noWrap sx={{ opacity: 0.85 }}>
                  {c.label}
                </Typography>
              )
            )}
            <Typography variant="h6" noWrap component="div" color="inherit">
              {title}
            </Typography>
          </Breadcrumbs>
        )}

        <Box flexGrow={1} />
        {features.security && <LayoutAuthMenu />}
      </Toolbar>
    </AppBar>
  );
};

export default LayoutAppBar;
