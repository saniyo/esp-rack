import React from 'react';
import {
  Divider,
  FormControl,
  InputLabel,
  ListItem,
  ListItemAvatar,
  ListItemText,
  MenuItem,
  Select,
  SelectChangeEvent
} from '@mui/material';
import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';
import { FieldAvatar } from '../utils/iconRegistry';
import { TIME_ZONES } from '../utils/timeZones';

interface TimeZonesProps {
  field: Field;
  value: string;
  onChange: (label: string, value: any) => void;
  onBlur?: (label: string, value: any) => void;
}

// Frontend-owned IANA timezone dropdown. Backend only knows the current label;
// on change we also write the sibling `tz_format` (POSIX TZ string) into the
// parent form state so it is submitted alongside `tz_label`.
const TimeZones: React.FC<TimeZonesProps> = ({ field, value, onChange }) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');
  const isReadOnly = !!optionMap.r;
  const colorName = (optionMap as any).color?.value as string | undefined;

  const current = value && TIME_ZONES[value as string] ? (value as string) : '';

  const handleChange = (event: SelectChangeEvent<string>) => {
    const newLabel = event.target.value;
    const fmt = TIME_ZONES[newLabel];
    if (!fmt) return;
    onChange(field.label, newLabel);
    onChange('tz_format', fmt);
  };

  if (isReadOnly) {
    return (
      <>
        <ListItem>
          <ListItemAvatar>
            <FieldAvatar iconName={field.icon} colorName={colorName} />
          </ListItemAvatar>
          <ListItemText primary={readableLabel} secondary={current || '—'} />
        </ListItem>
        <Divider variant="inset" component="li" />
      </>
    );
  }

  return (
    <ListItem>
      <FormControl fullWidth variant="outlined">
        <InputLabel>{readableLabel}</InputLabel>
        <Select
          value={current}
          onChange={handleChange}
          label={readableLabel}
        >
          <MenuItem disabled value="">Time zone...</MenuItem>
          {Object.keys(TIME_ZONES).map((label) => (
            <MenuItem key={label} value={label}>{label}</MenuItem>
          ))}
        </Select>
      </FormControl>
    </ListItem>
  );
};

export default TimeZones;
