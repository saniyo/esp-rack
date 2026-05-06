import { FC, useEffect, useState } from 'react';
import {
  Box,
  CircularProgress,
  Fade,
  LinearProgress,
  List,
  ListItem,
  ListItemIcon,
  ListItemText,
  Paper,
  Slide,
  Typography
} from '@mui/material';
import CheckCircleIcon from '@mui/icons-material/CheckCircle';
import RadioButtonUncheckedIcon from '@mui/icons-material/RadioButtonUnchecked';
import ErrorOutlineIcon from '@mui/icons-material/ErrorOutline';

import { FeatureEntry, UiManifest } from '../../types';

// Stagger between successive feature reveals, in ms. Tuned so a typical
// 15-20 module fleet finishes animating in ~1.2 s — long enough for the
// user to read what's loading, short enough that nobody waits on it.
const REVEAL_STAGGER_MS = 80;

interface ManifestProgressProps {
  loaded: boolean;
  manifest: UiManifest;
  error?: string;
  onRevealComplete: () => void;
}

// Drop entries that have no human-facing label — interior plumbing
// modules (Features, UiDyn, Presence) register manifest entries
// without a menu/title; surfacing their internal ids in the loader
// would only confuse the operator.
const visibleFeatures = (m: UiManifest): FeatureEntry[] =>
  m.features.filter((f) => (f.menu?.label || f.title) && !f.menu?.hidden);

const ManifestProgress: FC<ManifestProgressProps> = ({ loaded, manifest, error, onRevealComplete }) => {
  const features = visibleFeatures(manifest);
  const [revealed, setRevealed] = useState<number>(0);

  // Drive the staggered reveal once the fetch is done. Each tick exposes
  // one more feature row; when we've revealed them all (or the list is
  // empty), notify the parent so it can swap in the real app tree.
  // Error path: hold the message for ~1.6 s then release so the user
  // isn't trapped on the loader if the manifest endpoint is down — the
  // app falls back to its empty-manifest behavior behind the overlay.
  useEffect(() => {
    if (!loaded) return;
    if (error) {
      const t = setTimeout(onRevealComplete, 1600);
      return () => clearTimeout(t);
    }
    if (revealed >= features.length) {
      const t = setTimeout(onRevealComplete, 220);
      return () => clearTimeout(t);
    }
    const t = setTimeout(() => setRevealed((r) => r + 1), REVEAL_STAGGER_MS);
    return () => clearTimeout(t);
  }, [loaded, revealed, features.length, error, onRevealComplete]);

  const deviceName    = manifest.device?.name    || 'Device';
  const deviceVersion = manifest.device?.version;

  const phase: 'connecting' | 'enumerating' | 'error' =
    error ? 'error' : !loaded ? 'connecting' : 'enumerating';

  return (
    <Fade in timeout={200}>
      <Box
        sx={{
          position: 'fixed',
          inset: 0,
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
          bgcolor: 'background.default',
          zIndex: (t) => t.zIndex.modal + 1,
          p: 2
        }}
      >
        <Slide in direction="up" timeout={250}>
          <Paper elevation={8} sx={{ p: 3, width: '100%', maxWidth: 460, borderRadius: 2 }}>
            <Box sx={{ display: 'flex', alignItems: 'center', gap: 2, mb: 1 }}>
              {phase === 'error' ? (
                <ErrorOutlineIcon color="error" sx={{ fontSize: 36 }} />
              ) : (
                <CircularProgress
                  size={32}
                  thickness={4}
                  variant={phase === 'enumerating' ? 'determinate' : 'indeterminate'}
                  value={features.length === 0 ? 100 : Math.round((revealed / features.length) * 100)}
                />
              )}
              <Box sx={{ flex: 1, minWidth: 0 }}>
                <Typography variant="h6" noWrap>{deviceName}</Typography>
                {deviceVersion && (
                  <Typography variant="caption" color="text.secondary" noWrap>
                    {deviceVersion}
                  </Typography>
                )}
              </Box>
            </Box>

            <Typography variant="body2" color="text.secondary" sx={{ mb: 1.5 }}>
              {phase === 'connecting'  && 'Connecting to device…'}
              {phase === 'enumerating' && (
                features.length > 0
                  ? `Initializing modules (${Math.min(revealed, features.length)}/${features.length})`
                  : 'Ready'
              )}
              {phase === 'error'       && (error || 'Failed to load manifest')}
            </Typography>

            {phase !== 'error' && features.length > 0 && (
              <LinearProgress
                variant="determinate"
                value={Math.round((revealed / features.length) * 100)}
                sx={{ mb: 2, borderRadius: 1, height: 6 }}
              />
            )}

            {phase !== 'connecting' && features.length > 0 && (
              <List dense disablePadding sx={{ maxHeight: 320, overflowY: 'auto' }}>
                {features.map((f, idx) => {
                  const ready = idx < revealed;
                  return (
                    <ListItem
                      key={f.id}
                      disableGutters
                      sx={{
                        py: 0.25,
                        opacity: ready ? 1 : 0.5,
                        transition: 'opacity 180ms ease',
                      }}
                    >
                      <ListItemIcon sx={{ minWidth: 32 }}>
                        {ready ? (
                          <CheckCircleIcon color="success" fontSize="small" />
                        ) : (
                          <RadioButtonUncheckedIcon color="disabled" fontSize="small" />
                        )}
                      </ListItemIcon>
                      <ListItemText
                        primaryTypographyProps={{ variant: 'body2', noWrap: true }}
                        secondaryTypographyProps={{ variant: 'caption', noWrap: true }}
                        primary={f.menu?.label || f.title || f.id}
                        secondary={f.id}
                      />
                    </ListItem>
                  );
                })}
              </List>
            )}
          </Paper>
        </Slide>
      </Box>
    </Fade>
  );
};

export default ManifestProgress;
