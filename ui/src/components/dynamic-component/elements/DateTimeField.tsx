import React, { FC, useMemo, useState } from 'react';
import {
  ListItem,
  ListItemAvatar,
  ListItemText,
  TextField as MuiTextField,
} from '@mui/material';
import ScheduleIcon from '@mui/icons-material/Schedule';

import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';
import { FieldAvatar } from '../utils/iconRegistry';
import Popover from '@mui/material/Popover';
import Box from '@mui/material/Box';
import Button from '@mui/material/Button';
import Typography from '@mui/material/Typography';
import { useManifest } from '../../../contexts/manifest';
import { resolveZone } from '../../../utils/deviceTime';
import { fmtDate, fmtTime, monthGrid, WallParts, wallParts, wallToEpoch, zoneLabel } from '../utils/dateTimeParts';

interface DateTimeFieldProps {
  field: Field;
  value: number | string | undefined;  // Unix epoch seconds (int)
  onChange?: (label: string, value: number) => void;
  onBlur?: (label: string, value: number) => void;
}

// Unix-seconds → local-time ISO string suitable for HTML5 inputs.
// date:          "YYYY-MM-DD"
// time:          "HH:MM:SS"
// datetime-local "YYYY-MM-DDTHH:MM:SS"
const DateTimeField: FC<DateTimeFieldProps> = ({ field, value, onChange, onBlur }) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');
  const isReadOnly = !!optionMap.r || !!field.readOnly;
  const { manifest } = useManifest();
  const zone = useMemo(() => resolveZone(manifest.device?.tz), [manifest.device?.tz]);

  const rawMode = (optionMap as any).dtmode?.value as string | undefined;
  const timeOfDay = rawMode === 'tod';
  const mode: 'date' | 'time' | 'datetime' = rawMode === 'date' || rawMode === 'time' ? rawMode : 'datetime';
  const withDate = !timeOfDay && mode !== 'time';
  const withTime = timeOfDay || mode !== 'date';

  const numericSeconds = useMemo<number | undefined>(() => {
    if (value === undefined || value === null || value === '') return undefined;
    const n = typeof value === 'number' ? value : parseFloat(value as string);
    return Number.isFinite(n) ? n : undefined;
  }, [value]);

  // Time of day: seconds since midnight, no date and no zone.
  const todParts = (sec: number): WallParts =>
    ({ y: 1970, mo: 1, d: 1, h: Math.floor(sec / 3600) % 24, mi: Math.floor(sec / 60) % 60, s: 0 });
  const nowParts = (): WallParts => wallParts(Math.floor(Date.now() / 1000), zone);
  const partsOf = (sec?: number): WallParts | undefined => {
    if (sec === undefined) return undefined;
    return timeOfDay ? todParts(sec) : wallParts(sec, zone);
  };

  const shown = partsOf(numericSeconds);
  const text = !shown
    ? ''
    : [withDate ? fmtDate(shown) : '', withTime ? fmtTime(shown, !timeOfDay) : ''].filter(Boolean).join(' ');

  const [anchor, setAnchor] = useState<HTMLElement | null>(null);
  const [draft, setDraft] = useState<WallParts>(() => shown ?? nowParts());
  const [view, setView] = useState<{ y: number; mo: number }>({ y: draft.y, mo: draft.mo });

  const open = (el: HTMLElement) => {
    const d = shown ?? nowParts();
    setDraft(d);
    setView({ y: d.y, mo: d.mo });
    setAnchor(el);
  };
  const commit = () => {
    const sec = timeOfDay ? draft.h * 3600 + draft.mi * 60 : wallToEpoch(draft, zone);
    setAnchor(null);
    onChange?.(field.label, sec);
    onBlur?.(field.label, sec);
  };
  const setPart = (k: 'h' | 'mi' | 's', max: number) => (e: React.ChangeEvent<HTMLInputElement>) => {
    const n = Math.min(max, Math.max(0, parseInt(e.target.value || '0', 10) || 0));
    setDraft((d) => ({ ...d, [k]: n }));
  };
  const shiftMonth = (delta: number) =>
    setView((v) => {
      const m = v.mo - 1 + delta;
      return { y: v.y + Math.floor(m / 12), mo: ((m % 12) + 12) % 12 + 1 };
    });

  const avatar = (
    <ListItemAvatar>
      <FieldAvatar iconName={field.icon} fallback={ScheduleIcon} colorName={(optionMap as any).color?.value as string | undefined} />
    </ListItemAvatar>
  );

  if (isReadOnly) {
    return (
      <ListItem>
        {avatar}
        <ListItemText primary={readableLabel} secondary={text ? `${text}${timeOfDay ? '' : ` · ${zoneLabel(zone)}`}` : '—'} />
      </ListItem>
    );
  }

  const timeBox = (k: 'h' | 'mi' | 's', max: number, label: string) => (
    <MuiTextField
      label={label}
      type="number"
      value={draft[k]}
      onChange={setPart(k, max)}
      inputProps={{ min: 0, max, style: { width: 44 } }}
    />
  );

  return (
    <ListItem>
      {avatar}
      <MuiTextField
        fullWidth
        label={readableLabel}
        value={text}
        placeholder={withDate ? 'YYYY-MM-DD HH:MM:SS' : 'HH:MM'}
        onClick={(e) => open(e.currentTarget as HTMLElement)}
        InputLabelProps={{ shrink: true }}
        InputProps={{ readOnly: true }}
        helperText={timeOfDay ? undefined : zoneLabel(zone)}
      />
      <Popover
        open={!!anchor}
        anchorEl={anchor}
        onClose={() => setAnchor(null)}
        anchorOrigin={{ vertical: 'bottom', horizontal: 'left' }}
      >
        <Box sx={{ p: 1.5, width: 280 }}>
          {withDate && (
            <>
              <Box sx={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
                <Button size="small" onClick={() => shiftMonth(-1)}>‹</Button>
                <Typography variant="subtitle2">{`${view.y}-${String(view.mo).padStart(2, '0')}`}</Typography>
                <Button size="small" onClick={() => shiftMonth(1)}>›</Button>
              </Box>
              <Box sx={{ display: 'grid', gridTemplateColumns: 'repeat(7, 1fr)', gap: 0.25, textAlign: 'center' }}>
                {WEEKDAYS.map((w) => (
                  <Typography key={w} variant="caption" color="text.secondary">{w}</Typography>
                ))}
                {monthGrid(view.y, view.mo).map((d, i) =>
                  d ? (
                    <Button
                      key={i}
                      size="small"
                      sx={{ minWidth: 0, p: 0.25 }}
                      variant={draft.y === view.y && draft.mo === view.mo && draft.d === d ? 'contained' : 'text'}
                      onClick={() => setDraft((x) => ({ ...x, y: view.y, mo: view.mo, d }))}
                    >
                      {d}
                    </Button>
                  ) : (
                    <span key={i} />
                  ),
                )}
              </Box>
            </>
          )}
          {withTime && (
            <Box sx={{ display: 'flex', gap: 1, mt: 1 }}>
              {timeBox('h', 23, 'HH')}
              {timeBox('mi', 59, 'MM')}
              {!timeOfDay && timeBox('s', 59, 'SS')}
            </Box>
          )}
          {!timeOfDay && (
            <Typography variant="caption" color="text.secondary" component="div" sx={{ mt: 0.5 }}>
              {zoneLabel(zone)}
            </Typography>
          )}
          <Box sx={{ display: 'flex', justifyContent: 'space-between', mt: 1 }}>
            <Button size="small" onClick={() => setDraft(nowParts())}>Now</Button>
            <Box>
              <Button size="small" onClick={() => setAnchor(null)}>Cancel</Button>
              <Button size="small" variant="contained" onClick={commit}>Set</Button>
            </Box>
          </Box>
        </Box>
      </Popover>
    </ListItem>
  );
};

const WEEKDAYS = ['Mo', 'Tu', 'We', 'Th', 'Fr', 'Sa', 'Su'];

export default DateTimeField;
