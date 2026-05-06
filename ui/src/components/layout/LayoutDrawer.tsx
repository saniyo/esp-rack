import { FC } from 'react';

import { Box, Divider, Drawer, Toolbar, Typography, styled, Tooltip } from '@mui/material';

import { PROJECT_NAME } from '../../api/env';
import LayoutMenu from './LayoutMenu';
import { DRAWER_WIDTH } from './Layout';
import { useState, useContext, useEffect } from 'react';
import { FeaturesContext } from '../../contexts/features';
import { getSystemInfo, SystemInfo } from '../../api/system';

const LayoutDrawerLogo = styled('img')(({ theme }) => ({
  [theme.breakpoints.down("sm")]: {
    height: 24,
    marginRight: theme.spacing(2)
  },
  [theme.breakpoints.up("sm")]: {
    height: 36,
    marginRight: theme.spacing(2)
  }
}));

interface LayoutDrawerProps {
  mobileOpen: boolean;
  onClose: () => void;
}

const LayoutDrawer: FC<LayoutDrawerProps> = ({ mobileOpen, onClose }) => {
  const { features } = useContext(FeaturesContext);
  const [systemInfo, setSystemInfo] = useState<SystemInfo>();

  useEffect(() => {
    if (features.system_info) {
      getSystemInfo()
        .then((response) => setSystemInfo(response.data))
        .catch(console.error);
    }
  }, [features.system_info]);

  const versionString = systemInfo?.version
    ? (systemInfo.version.startsWith('v') ? systemInfo.version : `v${systemInfo.version}`)
    : '';

  const drawer = (
    <>
      <Toolbar disableGutters>
        <Box display="flex" alignItems="center" px={2} width="100%" position="relative">
          <LayoutDrawerLogo src="/app/icon.png" alt={PROJECT_NAME} />
          <Tooltip title={versionString} arrow placement="bottom">
            <Typography variant="h6" color="textPrimary">
              {PROJECT_NAME}
            </Typography>
          </Tooltip>
        </Box>
        <Divider absolute />
      </Toolbar>
      <Divider />
      <LayoutMenu />
    </>
  );

  return (
    <Box
      component="nav"
      sx={{ width: { md: DRAWER_WIDTH }, flexShrink: { md: 0 } }}
    >
      <Drawer
        variant="temporary"
        open={mobileOpen}
        onClose={onClose}
        ModalProps={{
          keepMounted: true, // Better open performance on mobile.
        }}
        sx={{
          display: { xs: 'block', md: 'none' },
          '& .MuiDrawer-paper': { boxSizing: 'border-box', width: DRAWER_WIDTH },
        }}
      >
        {drawer}
      </Drawer>
      <Drawer
        variant="permanent"
        sx={{
          display: { xs: 'none', md: 'block' },
          '& .MuiDrawer-paper': { boxSizing: 'border-box', width: DRAWER_WIDTH },
        }}
        open
      >
        {drawer}
      </Drawer>
    </Box>
  );

};

export default LayoutDrawer;
