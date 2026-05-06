import React, { FC } from 'react';
import { Box, Typography, keyframes } from '@mui/material';

// Pulsing ring used only when connected — signals active WS traffic without
// taking focus from the form. Disabled on the offline variant so it doesn't
// look like something's still trying to reach the server.
const pulse = keyframes`
  0%   { box-shadow: 0 0 0 0 rgba(76, 175, 80, 0.55); }
  70%  { box-shadow: 0 0 0 6px rgba(76, 175, 80, 0); }
  100% { box-shadow: 0 0 0 0 rgba(76, 175, 80, 0); }
`;

interface LiveIndicatorProps {
  connected: boolean;
}

// Tiny dot + label that slots into the top-right corner of a status panel.
// Appears the same regardless of backend (feature-agnostic) so users learn it
// once: green pulse = WS pushing, grey = REST snapshot only.
const LiveIndicator: FC<LiveIndicatorProps> = ({ connected }) => {
  return (
    <Box
      sx={{
        display: 'inline-flex',
        alignItems: 'center',
        gap: 0.75,
        px: 1,
        py: 0.25,
        borderRadius: 999,
        bgcolor: connected ? 'success.main' : 'action.disabledBackground',
        color: connected ? 'common.white' : 'text.secondary',
        fontSize: 12,
        lineHeight: 1,
        userSelect: 'none',
      }}
      aria-label={connected ? 'Live updates connected' : 'Live updates offline'}
    >
      <Box
        sx={{
          width: 8,
          height: 8,
          borderRadius: '50%',
          bgcolor: connected ? '#4caf50' : 'text.disabled',
          animation: connected ? `${pulse} 1.6s ease-out infinite` : 'none',
        }}
      />
      <Typography component="span" variant="caption" sx={{ fontWeight: 500 }}>
        {connected ? 'Live' : 'Offline'}
      </Typography>
    </Box>
  );
};

export default LiveIndicator;
