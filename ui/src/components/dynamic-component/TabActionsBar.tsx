import React, { FC } from 'react';
import { IconButton, Tooltip } from '@mui/material';
import RefreshIcon from '@mui/icons-material/Refresh';

import LiveIndicator from './LiveIndicator';
import { useDynamicForm } from './DynamicFormContext';

// Tab-level chrome rendered right-justified on the same row as the
// section title (consumed via SectionContent's `actions` slot). Carries
// a medium-sized Reload IconButton — invokes useDynamicForm().refetch()
// to GET the tab's restPath fresh, so operators can force a re-render
// of server-sourced fields without reopening the tab. Optionally also
// renders the Live/Offline indicator when the tab is WS-subscribed.
//
// Why uniform across live + REST: even WS tabs surface fields that
// don't push (slowly-changing config knobs that arrive only via REST
// at tab open). Without an explicit reload the operator's only option
// was to navigate away and back. Putting reload alongside the live
// indicator keeps one predictable affordance.
//
// Tinted icon-button style (`color="primary"`, larger size) so the
// control is visible against the heading row at a glance — small
// IconButton was easy to miss next to an h6 title.
interface TabActionsBarProps {
  // When provided, renders LiveIndicator alongside the reload button.
  // Pass `connected` for WS-bound tabs; omit for REST-only tabs.
  liveConnected?: boolean;
  // Heartbeat counter from useWs — bumped on every inbound frame.
  // Threaded through to LiveIndicator so the pulse animation fires
  // once per message rather than running unconditionally.
  messageTick?: number;
}

const TabActionsBar: FC<TabActionsBarProps> = ({ liveConnected, messageTick }) => {
  const { refetch } = useDynamicForm();
  const showLive = liveConnected !== undefined;

  return (
    <>
      <Tooltip title="Reload tab" arrow placement="left">
        <IconButton
          color="primary"
          onClick={() => refetch({ silent: false })}
          aria-label="Reload tab"
          sx={{
            bgcolor: 'action.hover',
            '&:hover': { bgcolor: 'action.selected' },
          }}
        >
          <RefreshIcon />
        </IconButton>
      </Tooltip>
      {showLive && <LiveIndicator connected={!!liveConnected} messageTick={messageTick} />}
    </>
  );
};

export default TabActionsBar;
