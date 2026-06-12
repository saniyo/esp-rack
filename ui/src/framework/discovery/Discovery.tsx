import { FC, useMemo } from 'react';
import {
  Box,
  Button,
  Card,
  CardContent,
  Chip,
  Stack,
  Tooltip,
  Typography,
} from '@mui/material';
import OpenInNewIcon from '@mui/icons-material/OpenInNew';
import RouterIcon from '@mui/icons-material/Router';
import VerifiedUserIcon from '@mui/icons-material/VerifiedUser';
import WarningIcon from '@mui/icons-material/Warning';

import { SectionContent, useLayoutTitle } from '../../components';
import { Peer, useDeviceMesh } from '../../utils/useDeviceMesh';

// Discovery — the consolidated LAN device overview (cards). The menu nests
// the navigable device records under this same "Discovery" umbrella; this
// page is the at-a-glance view with trust + compatibility detail. Each card
// links out to that device's own web UI.

const fmtAge = (ms: number): string => {
  const s = Math.max(0, Math.round(ms / 1000));
  if (s < 90) return `${s}s ago`;
  return `${Math.round(s / 60)}m ago`;
};

const PeerCard: FC<{ peer: Peer }> = ({ peer }) => {
  const open = () => window.open(`http://${peer.ip}/`, '_blank', 'noopener,noreferrer');
  return (
    <Card variant="outlined" sx={{ opacity: peer.compatible ? 1 : 0.6 }}>
      <CardContent>
        <Stack direction="row" justifyContent="space-between" alignItems="flex-start" spacing={1}>
          <Box sx={{ minWidth: 0 }}>
            <Typography variant="subtitle1" noWrap>{peer.project || '—'}</Typography>
            <Typography variant="caption" color="text.secondary" noWrap sx={{ display: 'block' }}>
              {peer.device_id}
            </Typography>
          </Box>
          <Tooltip title={peer.trusted ? 'Trusted — project key verified' : 'Untrusted — key mismatch'}>
            {peer.trusted ? <VerifiedUserIcon color="success" /> : <WarningIcon color="warning" />}
          </Tooltip>
        </Stack>

        <Stack direction="row" sx={{ mt: 1, flexWrap: 'wrap', gap: 0.5 }}>
          <Chip size="small" label={`fw ${peer.fw || '?'}`} />
          <Chip
            size="small"
            label={`schema ${peer.schema || '?'}`}
            color={peer.compatible ? 'success' : 'default'}
          />
          {!peer.compatible && <Chip size="small" color="warning" label="incompatible" />}
        </Stack>

        <Stack direction="row" justifyContent="space-between" alignItems="center" sx={{ mt: 1.5 }}>
          <Typography variant="body2" color="text.secondary" noWrap>
            {peer.ip}
            {peer.port && peer.port !== 80 ? `:${peer.port}` : ''} · {fmtAge(peer.age_ms)}
          </Typography>
          <Button size="small" endIcon={<OpenInNewIcon />} onClick={open}>
            Open
          </Button>
        </Stack>
      </CardContent>
    </Card>
  );
};

const PeerGrid: FC<{ items: Peer[] }> = ({ items }) => (
  <Box
    sx={{
      display: 'grid',
      gridTemplateColumns: { xs: '1fr', sm: '1fr 1fr', lg: '1fr 1fr 1fr' },
      gap: 2,
    }}
  >
    {items.map((p) => (
      <PeerCard key={p.device_id} peer={p} />
    ))}
  </Box>
);

const Discovery: FC = () => {
  useLayoutTitle('Discovery');
  const { self, peers } = useDeviceMesh(10000);

  const { same, others } = useMemo(() => {
    const selfProject = self?.project;
    const sort = (a: Peer, b: Peer) =>
      Number(b.trusted) - Number(a.trusted) || a.project.localeCompare(b.project);
    return {
      same: peers.filter((p) => p.project === selfProject).sort(sort),
      others: peers.filter((p) => p.project !== selfProject).sort(sort),
    };
  }, [peers, self]);

  return (
    <SectionContent title="Discovery" titleGutter>
      <Typography variant="body2" color="text.secondary" sx={{ mb: 2 }}>
        {peers.length} device(s) discovered on the LAN
      </Typography>

      {self && (
        <Card variant="outlined" sx={{ mb: 3, bgcolor: 'action.hover' }}>
          <CardContent>
            <Stack direction="row" spacing={1.5} alignItems="center">
              <RouterIcon color="primary" />
              <Box sx={{ minWidth: 0 }}>
                <Typography variant="subtitle2" noWrap>
                  This device · {self.project}
                </Typography>
                <Typography variant="caption" color="text.secondary" noWrap sx={{ display: 'block' }}>
                  {self.device_id} · {self.ip} · fw {self.fw} · schema {self.schema}
                </Typography>
              </Box>
            </Stack>
          </CardContent>
        </Card>
      )}

      {same.length > 0 && (
        <Box sx={{ mb: 3 }}>
          <Typography variant="overline" color="text.secondary">
            Same project ({same.length})
          </Typography>
          <Box sx={{ mt: 0.5 }}>
            <PeerGrid items={same} />
          </Box>
        </Box>
      )}

      {others.length > 0 && (
        <Box>
          <Typography variant="overline" color="text.secondary">
            Other projects ({others.length})
          </Typography>
          <Box sx={{ mt: 0.5 }}>
            <PeerGrid items={others} />
          </Box>
        </Box>
      )}

      {peers.length === 0 && (
        <Typography color="text.secondary" sx={{ mt: 2 }}>
          No other ESP-Rack devices found yet. Peers appear here within ~20&nbsp;seconds of
          joining the network.
        </Typography>
      )}
    </SectionContent>
  );
};

export default Discovery;
