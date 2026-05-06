import React, { FC, useMemo } from 'react';
import {
  ListItem,
  ListItemAvatar,
  ListItemText,
  Divider,
  TextField as MuiTextField,
} from '@mui/material';
import ScheduleIcon from '@mui/icons-material/Schedule';

import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';
import { FieldAvatar } from '../utils/iconRegistry';

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
function secondsToInputValue(
  seconds: number | undefined,
  mode: 'date' | 'time' | 'datetime'
): string {
  if (seconds === undefined || seconds === null || !Number.isFinite(seconds)) return '';
  const d = new Date(seconds * 1000);
  if (isNaN(d.getTime())) return '';
  const pad = (n: number) => String(n).padStart(2, '0');
  const y = d.getFullYear();
  const m = pad(d.getMonth() + 1);
  const day = pad(d.getDate());
  const h = pad(d.getHours());
  const min = pad(d.getMinutes());
  const s = pad(d.getSeconds());
  if (mode === 'date') return `${y}-${m}-${day}`;
  if (mode === 'time') return `${h}:${min}:${s}`;
  return `${y}-${m}-${day}T${h}:${min}:${s}`;
}

// Input string → Unix seconds (local-time interpretation).
function inputValueToSeconds(
  value: string,
  mode: 'date' | 'time' | 'datetime'
): number | undefined {
  if (!value) return undefined;
  let ms: number;
  if (mode === 'date') {
    const [y, m, d] = value.split('-').map(Number);
    ms = new Date(y, (m || 1) - 1, d || 1, 0, 0, 0).getTime();
  } else if (mode === 'time') {
    const [h, mm, ss] = value.split(':').map(Number);
    const now = new Date();
    now.setHours(h || 0, mm || 0, ss || 0, 0);
    ms = now.getTime();
  } else {
    ms = new Date(value).getTime();
  }
  if (isNaN(ms)) return undefined;
  return Math.floor(ms / 1000);
}

const inputTypeFor = (mode: 'date' | 'time' | 'datetime'): string => {
  if (mode === 'date') return 'date';
  if (mode === 'time') return 'time';
  return 'datetime-local';
};

const DateTimeField: FC<DateTimeFieldProps> = ({ field, value, onChange, onBlur }) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');
  const isReadOnly = !!optionMap.r || !!field.readOnly;

  const mode = useMemo<'date' | 'time' | 'datetime'>(() => {
    const raw = (optionMap as any).dtmode?.value as string | undefined;
    if (raw === 'date' || raw === 'time') return raw;
    return 'datetime';
  }, [optionMap]);

  const numericSeconds = useMemo<number | undefined>(() => {
    if (value === undefined || value === null || value === '') return undefined;
    const n = typeof value === 'number' ? value : parseFloat(value as string);
    return Number.isFinite(n) ? n : undefined;
  }, [value]);

  const inputValue = secondsToInputValue(numericSeconds, mode);

  const emit = (inputStr: string) => {
    const sec = inputValueToSeconds(inputStr, mode);
    if (sec === undefined) return;
    onChange?.(field.label, sec);
    onBlur?.(field.label, sec);
  };

  const secondaryReadonly = inputValue || '—';

  if (isReadOnly) {
    return (
      <>
        <ListItem>
          <ListItemAvatar>
            <FieldAvatar iconName={field.icon} fallback={ScheduleIcon} colorName={(optionMap as any).color?.value as string | undefined} />
          </ListItemAvatar>
          <ListItemText primary={readableLabel} secondary={secondaryReadonly} />
        </ListItem>
        <Divider variant="inset" component="li" />
      </>
    );
  }

  return (
    <ListItem>
      <ListItemAvatar>
        <FieldAvatar iconName={field.icon} fallback={ScheduleIcon} colorName={(optionMap as any).color?.value as string | undefined} />
      </ListItemAvatar>
      <MuiTextField
        fullWidth
        label={readableLabel}
        type={inputTypeFor(mode)}
        value={inputValue}
        onChange={(e) => emit(e.target.value)}
        onBlur={(e) => emit(e.target.value)}
        InputLabelProps={{ shrink: true }}
        inputProps={mode === 'time' ? { step: 1 } : { step: 1 }}
      />
    </ListItem>
  );
};

export default DateTimeField;
