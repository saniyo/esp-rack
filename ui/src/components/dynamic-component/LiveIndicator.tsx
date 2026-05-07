import React, { FC } from 'react';
import { Box, Typography, keyframes } from '@mui/material';

// One-shot heartbeat pulse — fires once per render, not infinitely.
// LiveIndicator re-keys the inner dot on every messageTick change,
// which forces a remount and replays the animation. Result: the dot
// blinks per actual inbound WS frame, idle when no traffic.
const pulse = keyframes`
  0%   { box-shadow: 0 0 0 0 rgba(76, 175, 80, 0.65); }
  70%  { box-shadow: 0 0 0 7px rgba(76, 175, 80, 0); }
  100% { box-shadow: 0 0 0 0 rgba(76, 175, 80, 0); }
`;

interface LiveIndicatorProps {
  connected: boolean;
  // Heartbeat counter from useWs — bumped per inbound WS frame. The
  // dot's React key is set to this value, so each bump remounts the
  // inner Box and the one-shot animation replays. When the stream
  // goes silent the dot stops blinking; when it resumes, it blinks
  // again — exactly matching what the operator can FEEL is alive.
  messageTick?: number;
}

const LiveIndicator: FC<LiveIndicatorProps> = ({ connected, messageTick }) => {
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
        // key forces remount on each messageTick → replay one-shot pulse.
        key={connected ? `tick-${messageTick ?? 0}` : 'offline'}
        sx={{
          width: 8,
          height: 8,
          borderRadius: '50%',
          bgcolor: connected ? '#4caf50' : 'text.disabled',
          animation: connected ? `${pulse} 0.8s ease-out 1` : 'none',
        }}
      />
      <Typography component="span" variant="caption" sx={{ fontWeight: 500 }}>
        {connected ? 'Live' : 'Offline'}
      </Typography>
    </Box>
  );
};

export default LiveIndicator;
