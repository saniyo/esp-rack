
import { FC, useState, useEffect } from 'react';
import { useLocation } from 'react-router-dom';

import { Box, Toolbar } from '@mui/material';

import { PROJECT_NAME } from '../../api/env';
import { useManifest } from '../../contexts/manifest';
import { RequiredChildrenProps } from '../../utils';

import LayoutDrawer from './LayoutDrawer';
import LayoutAppBar from './LayoutAppBar';
import { LayoutContext } from './context';

export const DRAWER_WIDTH = 280;

const Layout: FC<RequiredChildrenProps> = ({ children }) => {
  const { manifest } = useManifest();
  const brand = manifest.device?.name || PROJECT_NAME;

  const [mobileOpen, setMobileOpen] = useState(false);
  // `title` is the AppBar text. Initial value is the current brand;
  // sub-pages can override via setTitle (LayoutContext) when they
  // want to substitute their own label. Two re-sync hooks below
  // keep it honest:
  //   1. brand change — manifest landed / reloaded after sign-in →
  //      bump title back to the new brand. Without this, the very
  //      first useState capture of "ESP8266 React" (the stub manifest
  //      fallback to PROJECT_NAME) stayed visible forever even after
  //      the device name arrived.
  //   2. pathname change — when the user navigates between sections
  //      the inbound page picks its own setTitle in its first render;
  //      until that happens we reset to brand so the prior page's
  //      title doesn't linger.
  const [title, setTitle] = useState(brand);
  const { pathname } = useLocation();

  const handleDrawerToggle = () => {
    setMobileOpen(!mobileOpen);
  };

  useEffect(() => setMobileOpen(false), [pathname]);
  useEffect(() => setTitle(brand), [brand]);

  return (
    <LayoutContext.Provider value={{ title, setTitle }}>
      <LayoutAppBar title={title} onToggleDrawer={handleDrawerToggle} />
      <LayoutDrawer mobileOpen={mobileOpen} onClose={handleDrawerToggle} />
      <Box
        component="main"
        sx={{ marginLeft: { md: `${DRAWER_WIDTH}px` } }}
      >
        <Toolbar />
        {children}
      </Box>
    </LayoutContext.Provider >
  );

};

export default Layout;
