import React, { FC, useMemo } from 'react';
import { Box, Divider, LinearProgress, ListItem, ListItemAvatar, ListItemText, Tooltip } from '@mui/material';
import LinearScaleIcon from '@mui/icons-material/LinearScale';

import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';
import { FieldAvatar } from '../utils/iconRegistry';
import { ThresholdStep } from '../utils/fieldParser';
import { decimalsOf } from '../utils/formLayout';

// Read-only progress bar: value against mn..mx, where the max
// may come live from a sibling state key (maxKey) and a thin marker shows a
// sibling value such as a plan (markerKey). thr picks the bar colour by the
// percentage reached.
interface ProgressFieldProps {
  field: Field;
  value: any;
  getSibling?: (key: string) => any;
}

type BarColor = 'primary' | 'secondary' | 'error' | 'info' | 'success' | 'warning' | 'inherit';
const BAR_COLORS: readonly string[] = ['primary', 'secondary', 'error', 'info', 'success', 'warning', 'inherit'];

const num = (v: any): number | undefined => {
  const n = typeof v === 'number' ? v : typeof v === 'string' ? parseFloat(v) : NaN;
  return Number.isFinite(n) ? n : undefined;
};

const ProgressField: FC<ProgressFieldProps> = ({ field, value, getSibling }) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');
  const om = optionMap as any;
  const decimals = useMemo(() => decimalsOf(field.o || ''), [field.o]);
  const unit = (om.unit?.value as string) || '';

  const v = num(value);
  const min = num(om.mn?.value) ?? 0;
  const liveMax = om.maxKey?.value ? num(getSibling?.(om.maxKey.value as string)) : undefined;
  const max = liveMax !== undefined && liveMax > min ? liveMax : num(om.mx?.value) ?? 100;
  const marker = om.markerKey?.value ? num(getSibling?.(om.markerKey.value as string)) : undefined;

  const span = max - min;
  const pctOf = (x: number) => (span > 0 ? Math.min(100, Math.max(0, ((x - min) / span) * 100)) : 0);
  const pct = v !== undefined ? pctOf(v) : 0;
  const markerPct = marker !== undefined ? pctOf(marker) : undefined;

  const steps = (om.thr?.value as ThresholdStep[] | undefined) || [];
  let color: BarColor = 'primary';
  for (const s of steps) {
    if (pct >= s.pct && BAR_COLORS.includes(s.color)) color = s.color as BarColor;
  }

  const fmt = (x: number) => x.toFixed(decimals);
  const text =
    v === undefined
      ? '—'
      : `${fmt(v)} / ${fmt(max)}${unit ? ' ' + unit : ''} (${Math.round(pct)} %)` +
        (marker !== undefined ? ` · ${fmt(marker)}${unit ? ' ' + unit : ''} ⇣` : '');

  return (
    <>
      <ListItem>
        <ListItemAvatar>
          <FieldAvatar iconName={field.icon} fallback={LinearScaleIcon} colorName={om.color?.value as string | undefined} />
        </ListItemAvatar>
        <ListItemText
          primary={readableLabel}
          secondaryTypographyProps={{ component: 'div' }}
          secondary={
            <Box>
              <Box sx={{ position: 'relative', mt: 1, mb: 0.5 }}>
                <LinearProgress variant="determinate" value={pct} color={color} sx={{ height: 8, borderRadius: 1 }} />
                {markerPct !== undefined && (
                  <Tooltip title={`${fmt(marker as number)}${unit ? ' ' + unit : ''}`}>
                    <Box
                      sx={{
                        position: 'absolute',
                        top: -3,
                        left: `calc(${markerPct}% - 1px)`,
                        width: 2,
                        height: 14,
                        bgcolor: 'text.primary',
                      }}
                    />
                  </Tooltip>
                )}
              </Box>
              {text}
            </Box>
          }
        />
      </ListItem>
      <Divider variant="inset" component="li" />
    </>
  );
};

export default ProgressField;
