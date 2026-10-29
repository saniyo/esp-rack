import React, { FC } from 'react';
import { Box, Paper, Typography } from '@mui/material';

import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';
import { resolveIcon, sxColor } from '../utils/iconRegistry';
import { ColorMap } from '../utils/fieldParser';
import { decimalsOf } from '../utils/formLayout';

// Overview tile (.tile() on a Number / Text): the label as a caption, the
// value big with its unit, and a line under it from a sibling key (sub=).
// colorMap colours the value. Read-only; values come from the page's live
// state like any field's, the line from getSibling (as refLine does).
interface TileFieldProps {
  field: Field;
  value: any;
  getSibling?: (key: string) => any;
}

// The value as shown: a number at the field's format decimals (f=0.0),
// everything else as it came; '—' while there is none.
export const tileText = (value: any, o: string | undefined, type: string | undefined): string => {
  if (value === undefined || value === null || value === '') return '—';
  if (type === 'number' && /(?:^|;)f=/.test(o ?? '')) {
    const n = typeof value === 'number' ? value : parseFloat(value);
    if (Number.isFinite(n)) return n.toFixed(decimalsOf(o));
  }
  return String(value);
};

// colorMap: the shown text, then the raw value, then `default`.
export const tileColor = (map: ColorMap | undefined, shown: string, value: any): string | undefined => {
  if (!map) return undefined;
  return map[shown] ?? map[String(value ?? '')] ?? map['default'];
};

const TileField: FC<TileFieldProps> = ({ field, value, getSibling }) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');
  const om = optionMap as any;
  const unit = (om.unit?.value as string) || '';
  const subKey = om.sub?.value as string | undefined;
  const subRaw = subKey ? getSibling?.(subKey) : undefined;
  const sub = subRaw === undefined || subRaw === null ? '' : String(subRaw);
  const shown = tileText(value, field.o, field.type);
  const color = sxColor(tileColor(om.colorMap?.value as ColorMap | undefined, shown, value) ?? (om.color?.value as string));
  const Icon = resolveIcon(field.icon);

  return (
    <Box component="li" sx={{ height: '100%' }}>
      <Paper
        variant="outlined"
        sx={{ p: 1.5, height: '100%', boxSizing: 'border-box', display: 'flex', flexDirection: 'column', gap: 0.5, minWidth: 0 }}
      >
        <Box sx={{ display: 'flex', alignItems: 'center', gap: 0.75, color: 'text.secondary', minWidth: 0 }}>
          {Icon && <Icon sx={{ fontSize: 18 }} />}
          <Typography variant="body2" noWrap title={readableLabel}>
            {readableLabel}
          </Typography>
        </Box>
        <Box sx={{ display: 'flex', alignItems: 'baseline', flexWrap: 'wrap', columnGap: 0.5, minWidth: 0 }}>
          <Typography
            component="span"
            sx={{
              fontSize: { xs: '1.75rem', sm: 'clamp(1.25rem, 2.4vw, 2rem)' },
              fontWeight: 600,
              lineHeight: 1.2,
              color: color ?? 'text.primary',
              overflowWrap: 'anywhere',
            }}
          >
            {shown}
          </Typography>
          {unit && shown !== '—' && (
            <Typography component="span" variant="body1" color="text.secondary">
              {unit}
            </Typography>
          )}
        </Box>
        {sub && (
          <Typography variant="body2" color="text.secondary" sx={{ whiteSpace: 'pre-line', overflowWrap: 'anywhere' }}>
            {sub}
          </Typography>
        )}
      </Paper>
    </Box>
  );
};

export default TileField;
