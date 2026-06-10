import { FC, useCallback, useEffect, useMemo, useState } from 'react';
import {
  Box,
  Button,
  Card,
  CardContent,
  Chip,
  CircularProgress,
  IconButton,
  Stack,
  Tooltip,
  Typography,
} from '@mui/material';
import OpenInNewIcon from '@mui/icons-material/OpenInNew';
import RefreshIcon from '@mui/icons-material/Refresh';
import RouterIcon from '@mui/icons-material/Router';
import VerifiedUserIcon from '@mui/icons-material/VerifiedUser';
import WarningIcon from '@mui/icons-material/Warning';

import { SectionContent, useLayoutTitle } from '../../components';
import { AXIOS } from '../../api/endpoints';

// Devices — the consolidated LAN device directory. Fetches /rest/peers (the
// trust-verified mDNS peer list this device discovered) and renders the
// neighbours grouped by same-project vs other (cross-project) devices. Each
// card links out to that device's own web UI. (Phase 1 "directory"; in-panel
// rendering of a peer's manifest is the later gateway-proxy phase.)

interface Peer {
  device_id: string;
  project: string;
  fw: string;
  schema: string;
  framework: string;
  ip: string;
  port: number;
  trusted: boolean;
  compatible: boolean;
  age_ms: number;
}

interface SelfInfo {
  device_id: string;
  project: string;
  fw: string;
  schema: string;
  framework: string;
  ip: string;
}

interface PeersResponse {
  self: SelfInfo;
  peers: Peer[];
}

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

const Devices: FC = () => {
  useLayoutTitle('Devices');

  const [data, setData] = useState<PeersResponse | null>(null);
  const [error, setError] = useState<string | undefined>();
  const [loading, setLoading] = useState(false);

  const load = useCallback(() => {
    setLoading(true);
    AXIOS.get<PeersResponse>('peers')
      .then((r) => {
        setData(r.data);
        setError(undefined);
      })
      .catch((e) => setError(e?.message || 'Failed to load peers'))
      .finally(() => setLoading(false));
  }, []);

  useEffect(() => {
    load();
    const t = setInterval(load, 10000);
    return () => clearInterval(t);
  }, [load]);

  const { same, others } = useMemo(() => {
    const peers = data?.peers ?? [];
    const selfProject = data?.self.project;
    const sort = (a: Peer, b: Peer) =>
      Number(b.trusted) - Number(a.trusted) || a.project.localeCompare(b.project);
    return {
      same: peers.filter((p) => p.project === selfProject).sort(sort),
      others: peers.filter((p) => p.project !== selfProject).sort(sort),
    };
  }, [data]);

  const total = data?.peers?.length ?? 0;

  return (
    <SectionContent title="Devices" titleGutter>
      <Stack direction="row" alignItems="center" spacing={1} sx={{ mb: 2 }}>
        <Typography variant="body2" color="text.secondary" sx={{ flexGrow: 1 }}>
          {data ? `${total} device(s) discovered on the LAN` : 'Scanning the LAN…'}
        </Typography>
        {loading && <CircularProgress size={18} />}
        <Tooltip title="Rescan now">
          <IconButton onClick={load} size="small">
            <RefreshIcon />
          </IconButton>
        </Tooltip>
      </Stack>

      {error && (
        <Typography color="error" sx={{ mb: 2 }}>
          {error}
        </Typography>
      )}

      {data && (
        <Card variant="outlined" sx={{ mb: 3, bgcolor: 'action.hover' }}>
          <CardContent>
            <Stack direction="row" spacing={1.5} alignItems="center">
              <RouterIcon color="primary" />
              <Box sx={{ minWidth: 0 }}>
                <Typography variant="subtitle2" noWrap>
                  This device · {data.self.project}
                </Typography>
                <Typography variant="caption" color="text.secondary" noWrap sx={{ display: 'block' }}>
                  {data.self.device_id} · {data.self.ip} · fw {data.self.fw} · schema {data.self.schema}
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

      {data && total === 0 && (
        <Typography color="text.secondary" sx={{ mt: 2 }}>
          No other ESP-Rack devices found yet. Peers appear here within ~20&nbsp;seconds of
          joining the network.
        </Typography>
      )}
    </SectionContent>
  );
};

export default Devices;
