// TrendHistoryBar: a history-capable Trend's header. Title and a status line,
// the period switch (Live | 24 h | 7 d | 30 d | 1 y | a day), Reload and CSV
// in a history period, the day picker and the folded notes. Below 640 px of
// chart width the switch takes its own full-width row.
import React, { useEffect, useRef, useState } from 'react';
import {
  Box, IconButton, ListItem, ListItemAvatar, ToggleButton, ToggleButtonGroup, Tooltip, Typography,
} from '@mui/material';
import RefreshIcon from '@mui/icons-material/Refresh';
import EventIcon from '@mui/icons-material/Event';
import FileDownloadIcon from '@mui/icons-material/FileDownload';
import InfoOutlinedIcon from '@mui/icons-material/InfoOutlined';

import { FieldAvatar } from '../../utils/iconRegistry';
import { wallParts, wallToEpoch } from '../../utils/dateTimeParts';
import { HistPeriod } from './historyPlan';
import { HistoryState } from './useHistoryData';

const LABEL: Record<HistPeriod, string> = { live: 'Live', '24h': '24 h', '7d': '7 d', '30d': '30 d', '1y': '1 y', date: '' };
const WIDE = 640;

const pad = (n: number) => String(n).padStart(2, '0');

interface Props {
  title: string;
  iconName?: string;
  periods: HistPeriod[];
  liveLabel: string;
  liveSince?: number;  // epoch s the Live data starts at (.liveSince())
  h: HistoryState;
}

const TrendHistoryBar: React.FC<Props> = ({ title, iconName, periods, liveLabel, liveSince, h }) => {
  const boxRef = useRef<HTMLDivElement>(null);
  const dateRef = useRef<HTMLInputElement>(null);
  const [width, setWidth] = useState(WIDE);
  const [openNotes, setOpenNotes] = useState(false);

  useEffect(() => {
    const el = boxRef.current;
    if (!el || typeof ResizeObserver === 'undefined') return;
    const ro = new ResizeObserver((e) => setWidth(e[0].contentRect.width));
    ro.observe(el);
    return () => ro.disconnect();
  }, []);
  const wide = width >= WIDE;

  const ymd = (t: number) => {
    const w = wallParts(t, h.zone);
    return `${w.y}-${pad(w.mo)}-${pad(w.d)}`;
  };
  const dm = (t: number) => {
    const w = wallParts(t, h.zone);
    return `${pad(w.d)}.${pad(w.mo)}`;
  };
  const dmy = (t: number) => `${dm(t)}.${wallParts(t, h.zone).y}`;
  const hm = (t: number) => {
    const w = wallParts(t, h.zone);
    return `${pad(w.h)}:${pad(w.mi)}`;
  };

  const openPicker = () => {
    const el = dateRef.current;
    if (!el) return;
    try {
      (el as any).showPicker();
    } catch {
      el.style.position = 'static';
      el.style.opacity = '1';
      el.focus();
    }
  };
  const onDate = (v: string) => {
    const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(v);
    if (!m) return;
    h.setDay(wallToEpoch({ y: +m[1], mo: +m[2], d: +m[3], h: 0, mi: 0, s: 0 }, h.zone));
  };

  const switcher = (
    <ToggleButtonGroup
      size="small"
      exclusive
      value={h.period}
      onChange={(_, v: HistPeriod | null) => {
        if (!v) return;
        h.setPeriod(v);
        if (v === 'date') window.setTimeout(openPicker, 0);
      }}
      sx={wide ? undefined : { width: '100%', '& .MuiToggleButton-root': { flex: '1 1 auto', px: 0.5 } }}
    >
      {periods.map((p) => (
        <ToggleButton key={p} value={p} sx={{ textTransform: 'none', py: 0.25 }}>
          {p === 'date' ? (
            <>
              <EventIcon fontSize="small" />
              {wide && h.period === 'date' && h.day !== undefined ? <Box component="span" sx={{ ml: 0.5 }}>{dm(h.day)}</Box> : null}
            </>
          ) : p === 'live' ? (
            liveLabel
          ) : (
            LABEL[p]
          )}
        </ToggleButton>
      ))}
    </ToggleButtonGroup>
  );

  const actions = h.active ? (
    <>
      {h.csv && (
        <Tooltip title="Download these records as CSV">
          <IconButton size="small" onClick={h.csv}>
            <FileDownloadIcon fontSize="small" />
          </IconButton>
        </Tooltip>
      )}
      <Tooltip title="Reload (history is a snapshot)">
        <IconButton size="small" onClick={h.reload}>
          <RefreshIcon fontSize="small" />
        </IconButton>
      </Tooltip>
    </>
  ) : null;

  const status = h.active ? (
    h.status
  ) : (
    <Box component="span" sx={{ display: 'inline-flex', alignItems: 'center', gap: 0.75 }}>
      <Box
        component="span"
        sx={{
          width: 8,
          height: 8,
          borderRadius: '50%',
          bgcolor: 'success.main',
          animation: 'trendLivePulse 2s ease-in-out infinite',
          '@keyframes trendLivePulse': { '0%, 100%': { opacity: 1 }, '50%': { opacity: 0.35 } },
        }}
      />
      {liveLabel}
      {liveSince !== undefined ? ` · since ${dmy(liveSince)} ${hm(liveSince)}` : ''}
    </Box>
  );

  return (
    <ListItem sx={{ display: 'block' }}>
      <Box ref={boxRef} sx={{ display: 'flex', alignItems: 'center', gap: 1, minWidth: 0 }}>
        <ListItemAvatar sx={{ minWidth: 48 }}>
          <FieldAvatar iconName={iconName} />
        </ListItemAvatar>
        <Box sx={{ flex: '1 1 auto', minWidth: 0 }}>
          <Typography variant="body1" noWrap>
            {title}
          </Typography>
          <Typography variant="body2" color="text.secondary" noWrap>
            {status}
          </Typography>
        </Box>
        {wide && switcher}
        {actions}
      </Box>
      {!wide && <Box sx={{ mt: 1 }}>{switcher}</Box>}

      {h.active && h.period === 'date' && (
        <Box sx={{ mt: 1, display: 'flex', alignItems: 'center', gap: 1 }}>
          <Typography variant="body2" color="text.secondary">
            Day
          </Typography>
          <Box
            component="button"
            type="button"
            onClick={openPicker}
            sx={{ font: 'inherit', px: 1, py: 0.25, border: 1, borderColor: 'divider', borderRadius: 1, bgcolor: 'transparent', color: 'text.primary', cursor: 'pointer' }}
          >
            {h.day !== undefined ? dmy(h.day) : '—'}
          </Box>
          <input
            ref={dateRef}
            type="date"
            value={h.day !== undefined ? ymd(h.day) : ''}
            min={h.minDay !== undefined ? ymd(h.minDay) : undefined}
            max={ymd(h.maxDay)}
            onChange={(e) => onDate(e.target.value)}
            style={{ position: 'absolute', opacity: 0, width: 1, height: 1, pointerEvents: 'none' }}
            aria-label="Day"
          />
        </Box>
      )}

      {h.active && h.notes.length > 0 && (
        <Box
          onClick={() => setOpenNotes((o) => !o)}
          sx={{ mt: 0.5, display: 'flex', alignItems: 'flex-start', gap: 0.5, cursor: h.notes.length > 1 ? 'pointer' : 'default', color: 'text.secondary' }}
        >
          <InfoOutlinedIcon sx={{ fontSize: 16, mt: '2px' }} />
          {openNotes ? (
            <Box>
              {h.notes.map((n) => (
                <Typography key={n} variant="caption" component="div">
                  {n}
                </Typography>
              ))}
            </Box>
          ) : (
            <Typography variant="caption" noWrap sx={{ minWidth: 0 }}>
              {h.notes[0]}
              {h.notes.length > 1 ? `  +${h.notes.length - 1}` : ''}
            </Typography>
          )}
        </Box>
      )}
    </ListItem>
  );
};

export default TrendHistoryBar;
