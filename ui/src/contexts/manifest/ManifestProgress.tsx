import { FC, useEffect, useMemo, useState } from 'react';
import {
  Box,
  CircularProgress,
  Fade,
  LinearProgress,
  Paper,
  Slide,
  Typography
} from '@mui/material';
import CheckCircleIcon from '@mui/icons-material/CheckCircle';
import RadioButtonUncheckedIcon from '@mui/icons-material/RadioButtonUnchecked';
import ErrorOutlineIcon from '@mui/icons-material/ErrorOutline';
import WarningAmberIcon from '@mui/icons-material/WarningAmber';

import { AXIOS } from '../../api/endpoints';
import { FeatureEntry, UiManifest } from '../../types';

// Per-module probe timeout. We're not measuring server latency, just
// confirming the endpoint actually responds (any 2xx/4xx is "loaded"
// — only network failures / 5xx / timeout count as fail). 3 s
// accommodates the worst-case TLS handshake on the slowest currently-
// shipped target (ESP32-C3) without leaving the splash screen feeling
// stuck.
const PROBE_TIMEOUT_MS = 3000;
// Minimum dwell on each "in-flight" row even if the probe resolves
// instantly. Gives the operator's eye time to read what's loading
// before the carousel scrolls — without this a cached probe response
// makes the row flash through in 30 ms.
const MIN_DWELL_MS = 280;
// Pause after the last row before handing control to the real app
// tree. Lets the final green check register visually.
const FINAL_HOLD_MS = 400;
// Carousel geometry — height of one row + the vertical slot each row
// is anchored at within the 3-row viewport.
const ROW_HEIGHT_PX = 56;
const ROW_GAP_PX    = 4;
const SLOT_PITCH    = ROW_HEIGHT_PX + ROW_GAP_PX;
const VIEWPORT_PX   = SLOT_PITCH * 3;

// Modules without a UI surface (interior plumbing: features, ui-dyn,
// presence) carry no menu label or title — surfacing their internal
// ids would only confuse the operator.
const visibleFeatures = (m: UiManifest): FeatureEntry[] =>
  m.features.filter((f) => (f.menu?.label || f.title) && !f.menu?.hidden);

// What URL we actually GET to confirm a feature is alive. Prefer the
// declared rest.read; fall back to the first tab's restPath; null
// means "no probe possible, mark as skipped".
const probeUrlFor = (f: FeatureEntry): string | null => {
  if (f.rest?.read) return f.rest.read;
  if (f.tabs && f.tabs[0]?.restPath) return f.tabs[0].restPath;
  return null;
};

type ProbeStatus = 'pending' | 'loading' | 'ok' | 'fail' | 'skip';

interface ManifestProgressProps {
  loaded: boolean;
  manifest: UiManifest;
  error?: string;
  onRevealComplete: () => void;
}

// Single row of the carousel — the data is the same shape whether the
// row is the focused middle or one of the dimmed neighbours; the
// `focused` flag drives styling (size, weight, opacity).
const CarouselRow: FC<{
  feature: FeatureEntry;
  status: ProbeStatus;
  focused: boolean;
  top: number;
}> = ({ feature, status, focused, top }) => {
  const statusIcon = (() => {
    switch (status) {
      case 'ok':
        return <CheckCircleIcon color="success" fontSize="small" />;
      case 'fail':
        return <ErrorOutlineIcon color="error" fontSize="small" />;
      case 'skip':
        return <WarningAmberIcon color="warning" fontSize="small" />;
      case 'loading':
        return <CircularProgress size={18} thickness={5} />;
      case 'pending':
      default:
        return <RadioButtonUncheckedIcon color="disabled" fontSize="small" />;
    }
  })();

  return (
    <Box
      sx={{
        position: 'absolute',
        left: 0,
        right: 0,
        top: `${top}px`,
        height: `${ROW_HEIGHT_PX}px`,
        display: 'flex',
        alignItems: 'center',
        gap: 1.5,
        px: 2,
        borderRadius: 1.5,
        bgcolor: focused ? 'action.selected' : 'transparent',
        opacity: focused ? 1 : 0.45,
        transform: focused ? 'scale(1)' : 'scale(0.92)',
        transformOrigin: 'left center',
        transition:
          'top 300ms cubic-bezier(0.22, 0.61, 0.36, 1), ' +
          'opacity 300ms ease, ' +
          'transform 300ms ease, ' +
          'background-color 300ms ease',
      }}
    >
      <Box
        sx={{
          width: 22,
          height: 22,
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
        }}
      >
        {statusIcon}
      </Box>
      <Box sx={{ flex: 1, minWidth: 0 }}>
        <Typography
          noWrap
          sx={{
            fontWeight: focused ? 600 : 400,
            fontSize: focused ? '1rem' : '0.875rem',
            transition: 'font-size 300ms ease, font-weight 300ms ease',
          }}
        >
          {feature.menu?.label || feature.title || feature.id}
        </Typography>
        <Typography
          variant="caption"
          color="text.secondary"
          noWrap
          sx={{ display: 'block' }}
        >
          {feature.id}
        </Typography>
      </Box>
    </Box>
  );
};

const ManifestProgress: FC<ManifestProgressProps> = ({
  loaded,
  manifest,
  error,
  onRevealComplete,
}) => {
  const features = useMemo(() => visibleFeatures(manifest), [manifest]);
  const total = features.length;

  // idx points at the module currently being probed. Initial -1 keeps
  // the carousel below the first row until the manifest is loaded and
  // we kick the first probe (which advances idx to 0). When idx ===
  // total the whole list is done and we hand off to the app tree.
  const [idx, setIdx] = useState<number>(-1);
  const [statuses, setStatuses] = useState<ProbeStatus[]>([]);

  // Re-initialise status array whenever the manifest's feature count
  // changes. Reset idx back to -1 so the first probe re-fires on the
  // new list (e.g. after a manifest reload following sign-in).
  useEffect(() => {
    setStatuses(features.map(() => 'pending'));
    setIdx(total > 0 ? 0 : -1);
  }, [total, features]);

  // Probe one module at a time. The effect refires on every idx
  // change; the cleanup aborts the in-flight fetch so an unmount
  // mid-probe doesn't try to setState on a dead component.
  useEffect(() => {
    if (!loaded || error) return;
    if (total === 0) return;
    if (idx < 0 || idx >= total) return;

    setStatuses((prev) => {
      const next = prev.slice();
      next[idx] = 'loading';
      return next;
    });

    const feature = features[idx];
    const url = probeUrlFor(feature);

    let aborted = false;
    let advanceTimer: number | undefined;

    const scheduleAdvance = (result: ProbeStatus) => {
      if (aborted) return;
      setStatuses((prev) => {
        const next = prev.slice();
        next[idx] = result;
        return next;
      });
      advanceTimer = window.setTimeout(() => {
        if (!aborted) setIdx((i) => i + 1);
      }, MIN_DWELL_MS);
    };

    if (!url) {
      // Nothing to probe — page-type features or features that only
      // expose actions. Mark as skipped (warning icon) and advance.
      scheduleAdvance('skip');
      return () => {
        aborted = true;
        if (advanceTimer) clearTimeout(advanceTimer);
      };
    }

    const controller = new AbortController();
    const timeoutId = window.setTimeout(
      () => controller.abort(),
      PROBE_TIMEOUT_MS
    );

    AXIOS.get(url, { signal: controller.signal })
      .then(() => {
        clearTimeout(timeoutId);
        scheduleAdvance('ok');
      })
      .catch((err) => {
        clearTimeout(timeoutId);
        // Treat 4xx as "endpoint responded" — auth-required modules
        // commonly 401 here and we still want to claim them as loaded.
        const status = err?.response?.status;
        if (typeof status === 'number' && status >= 400 && status < 500) {
          scheduleAdvance('ok');
        } else {
          scheduleAdvance('fail');
        }
      });

    return () => {
      aborted = true;
      controller.abort();
      clearTimeout(timeoutId);
      if (advanceTimer) clearTimeout(advanceTimer);
    };
  }, [loaded, error, idx, total, features]);

  // Hand off to the real app tree when we've walked off the end of
  // the feature list (or the manifest failed entirely).
  useEffect(() => {
    if (!loaded) return;
    if (error) {
      const t = setTimeout(onRevealComplete, 1600);
      return () => clearTimeout(t);
    }
    if (total === 0 || idx >= total) {
      const t = setTimeout(onRevealComplete, FINAL_HOLD_MS);
      return () => clearTimeout(t);
    }
  }, [loaded, error, idx, total, onRevealComplete]);

  const deviceName    = manifest.device?.name    || 'Device';
  const deviceVersion = manifest.device?.version;

  const phase: 'connecting' | 'enumerating' | 'error' = error
    ? 'error'
    : !loaded
    ? 'connecting'
    : 'enumerating';

  // The carousel renders up to 3 rows (prev / current / next) at fixed
  // absolute top offsets within the viewport. Because each Row's key
  // is its feature.id, React preserves the same DOM nodes across idx
  // changes — the only thing that changes is each row's `top` (and the
  // `focused` flag at the slot transition), which animates via CSS
  // transition. Result: the wheel rolls smoothly without a re-mount
  // flash on every step.
  const slots = [
    idx > 0          ? features[idx - 1] : null,   // top — just done
    idx >= 0 && idx < total ? features[idx]     : null,   // middle — current
    idx + 1 < total  ? features[idx + 1] : null,   // bottom — queued
  ];

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
          p: 2,
        }}
      >
        <Slide in direction="up" timeout={250}>
          <Paper
            elevation={8}
            sx={{ p: 3, width: '100%', maxWidth: 480, borderRadius: 2 }}
          >
            <Box sx={{ display: 'flex', alignItems: 'center', gap: 2, mb: 1 }}>
              {phase === 'error' ? (
                <ErrorOutlineIcon color="error" sx={{ fontSize: 36 }} />
              ) : (
                <CircularProgress
                  size={32}
                  thickness={4}
                  variant={
                    phase === 'enumerating' && total > 0
                      ? 'determinate'
                      : 'indeterminate'
                  }
                  value={
                    total === 0
                      ? 100
                      : Math.min(
                          100,
                          Math.round((Math.max(idx, 0) / total) * 100)
                        )
                  }
                />
              )}
              <Box sx={{ flex: 1, minWidth: 0 }}>
                <Typography variant="h6" noWrap>
                  {deviceName}
                </Typography>
                {deviceVersion && (
                  <Typography
                    variant="caption"
                    color="text.secondary"
                    noWrap
                  >
                    {deviceVersion}
                  </Typography>
                )}
              </Box>
            </Box>

            <Typography
              variant="body2"
              color="text.secondary"
              sx={{ mb: 1.5 }}
            >
              {phase === 'connecting'  && 'Connecting to device…'}
              {phase === 'enumerating' &&
                (total > 0
                  ? `Probing modules (${Math.min(idx + 1, total)}/${total})`
                  : 'Ready')}
              {phase === 'error' && (error || 'Failed to load manifest')}
            </Typography>

            {phase !== 'error' && total > 0 && (
              <LinearProgress
                variant="determinate"
                value={Math.min(
                  100,
                  Math.round((Math.max(idx, 0) / total) * 100)
                )}
                sx={{ mb: 2, borderRadius: 1, height: 6 }}
              />
            )}

            {phase !== 'connecting' && total > 0 && (
              <Box
                sx={{
                  position: 'relative',
                  height: `${VIEWPORT_PX}px`,
                  overflow: 'hidden',
                  // Soft top/bottom fade so the non-focused rows look
                  // like they're rolling in/out of view.
                  maskImage:
                    'linear-gradient(to bottom, transparent 0%, ' +
                    'black 18%, black 82%, transparent 100%)',
                  WebkitMaskImage:
                    'linear-gradient(to bottom, transparent 0%, ' +
                    'black 18%, black 82%, transparent 100%)',
                }}
              >
                {slots.map((feature, slotIdx) => {
                  if (!feature) return null;
                  const status = statuses[idx + slotIdx - 1] || 'pending';
                  return (
                    <CarouselRow
                      key={feature.id}
                      feature={feature}
                      status={status}
                      focused={slotIdx === 1}
                      top={slotIdx * SLOT_PITCH}
                    />
                  );
                })}
              </Box>
            )}
          </Paper>
        </Slide>
      </Box>
    </Fade>
  );
};

export default ManifestProgress;
