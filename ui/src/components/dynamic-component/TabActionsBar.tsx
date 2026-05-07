import React, { FC } from 'react';
import { Box, IconButton, Tooltip } from '@mui/material';
import RefreshIcon from '@mui/icons-material/Refresh';

import LiveIndicator from './LiveIndicator';
import { useDynamicForm } from './DynamicFormContext';

// Top-right slot that every tab gets. Always carries a Reload button
// — invokes useDynamicForm().refetch() to GET the tab's restPath
// fresh, so operators can force a re-render of fields whose values
// originate server-side without reopening the tab. Optionally also
// renders the Live/Offline indicator when the tab is WS-subscribed.
//
// Why uniform across live + REST: even WS tabs surface fields that
// don't push (slowly-changing config knobs that arrive only via REST
// at tab open). Without an explicit reload the operator's only option
// was to navigate away and back, which is awkward. Putting a reload
// next to the live indicator keeps a single, predictable affordance.
interface TabActionsBarProps {
  // When provided, renders LiveIndicator alongside the reload button.
  // Pass `connected` for WS-bound tabs; omit for REST-only tabs.
  liveConnected?: boolean;
}

const TabActionsBar: FC<TabActionsBarProps> = ({ liveConnected }) => {
  const { refetch } = useDynamicForm();
  const showLive = liveConnected !== undefined;

  return (
    <Box sx={{ display: 'flex', justifyContent: 'flex-end', alignItems: 'center', gap: 1, mb: 1 }}>
      <Tooltip title="Reload tab" arrow placement="left">
        <IconButton
          size="small"
          onClick={() => refetch({ silent: false })}
          aria-label="Reload tab"
        >
          <RefreshIcon fontSize="small" />
        </IconButton>
      </Tooltip>
      {showLive && <LiveIndicator connected={!!liveConnected} />}
    </Box>
  );
};

export default TabActionsBar;
