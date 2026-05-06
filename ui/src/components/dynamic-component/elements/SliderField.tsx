import React, { useEffect, useMemo, useState, useCallback } from 'react';
import {
  ListItem,
  Divider,
  ListItemAvatar,
  ListItemText,
  Box,
} from '@mui/material';
import Slider from '@mui/material/Slider';
import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';
import { FieldAvatar } from '../utils/iconRegistry';

interface SliderFieldProps {
  field: Field;
  value: number | string | undefined;
  onChange: (label: string, value: number) => void;
  onBlur?: (label: string, value: number) => void;
}

const SliderField: React.FC<SliderFieldProps> = ({
  field,
  value,
  onChange,
  onBlur,
}) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');

  const isReadOnly = !!optionMap.r;

  const minValue =
    Number.isFinite(Number((field as any).min))
      ? Number((field as any).min)
      : (Number.isFinite(Number(optionMap.mn?.value)) ? Number(optionMap.mn!.value) : 0);

  const maxValue =
    Number.isFinite(Number((field as any).max))
      ? Number((field as any).max)
      : (Number.isFinite(Number(optionMap.mx?.value)) ? Number(optionMap.mx!.value) : 100);

  const step =
    Number.isFinite(Number((field as any).step)) && Number((field as any).step) > 0
      ? Number((field as any).step)
      : (Number.isFinite(Number(optionMap.st?.value)) && Number(optionMap.st!.value) > 0
        ? Number(optionMap.st!.value)
        : 1);

  const toNum = (v: unknown): number => {
    if (typeof v === 'number') return Number.isFinite(v) ? v : NaN;
    if (typeof v === 'string') {
      const t = v.trim();
      if (!t) return NaN;
      const n = Number(t.replace(',', '.'));
      return Number.isFinite(n) ? n : NaN;
    }
    return NaN;
  };

  const numericValue = toNum(value);
  const clamp = useCallback((n: number) => Math.min(maxValue, Math.max(minValue, n)), [maxValue, minValue]);

  const [internalValue, setInternalValue] = useState<number>(
    Number.isFinite(numericValue) ? clamp(numericValue) : minValue
  );

  const [isEditing, setIsEditing] = useState(false);

  useEffect(() => {
    if (!isEditing) {
      const n = toNum(value);
      setInternalValue(Number.isFinite(n) ? clamp(n) : minValue);
    }
  }, [value, isEditing, minValue, maxValue, clamp]);

  const marks = useMemo(() => ([
    { value: minValue, label: String(minValue) },
    { value: maxValue, label: String(maxValue) }
  ]), [minValue, maxValue]);

  const handleChange = (_: Event, newVal: number | number[]) => {
    const v = clamp(Array.isArray(newVal) ? newVal[0] : newVal);
    setInternalValue(v);
    onChange && onChange(field.label, v);
  };

  const handleChangeCommitted = (_: React.SyntheticEvent | Event, newVal: number | number[]) => {
    const v = clamp(Array.isArray(newVal) ? newVal[0] : newVal);
    setIsEditing(false);
    onBlur && onBlur(field.label, v);
  };

  if (isReadOnly) {
    return (
      <>
        <ListItem sx={{ alignItems: "center" }}>
          <ListItemAvatar>
            <FieldAvatar iconName={field.icon} colorName={(optionMap as any).color?.value as string | undefined} />
          </ListItemAvatar>
          <ListItemText
            primary={readableLabel}
            secondary={value}
            sx={{
              flex: "0 0 auto", pr: 2
            }}
          />

          <Box sx={{ flex: 1, minWidth: 0 }}>
            <Box mt={1}>
              <Slider
                sx={{
                  '& .MuiSlider-markLabel': {
                    fontSize: '0.7em',
                    color: 'text.secondary',
                  },
                  '& .MuiSlider-markLabelActive': {
                    fontSize: '0.7em',
                    color: 'text.secondary',
                  },
                  '&.Mui-disabled .MuiSlider-markLabel, &.Mui-disabled .MuiSlider-markLabelActive': {
                    color: (t) => t.palette.text.disabled,
                  },
                }}
                value={internalValue}
                min={minValue}
                max={maxValue}
                step={step}
                marks={marks}
                valueLabelDisplay="auto"
                onChange={handleChange}
                onChangeCommitted={handleChangeCommitted}
                aria-label={readableLabel}
                disabled
              />
            </Box>
          </Box>
        </ListItem>

        <Divider variant="inset" component="li" />
      </>
    );
  }

  return (
    <>
      <ListItem sx={{ alignItems: "center" }}>
        <ListItemAvatar>
          <FieldAvatar iconName={field.icon} colorName={(optionMap as any).color?.value as string | undefined} />
        </ListItemAvatar>
        <ListItemText
          primary={readableLabel}
          secondary={value}
          sx={{
            flex: "0 0 auto", pr: 2
          }}
        />

        <Box sx={{ flex: 1, minWidth: 0 }}>
          <Box mt={1}>
            <Slider
              sx={{
                '& .MuiSlider-markLabel': {
                  fontSize: '0.7em',
                  color: 'text.secondary',
                },
                '& .MuiSlider-markLabelActive': {
                  fontSize: '0.7em',
                  color: 'text.secondary',
                },
                '&.Mui-disabled .MuiSlider-markLabel, &.Mui-disabled .MuiSlider-markLabelActive': {
                  color: (t) => t.palette.text.disabled,
                },
              }}
              value={internalValue}
              min={minValue}
              max={maxValue}
              step={step}
              marks={marks}
              valueLabelDisplay="auto"
              onChange={handleChange}
              onChangeCommitted={handleChangeCommitted}
              aria-label={readableLabel}
            />
          </Box>
        </Box>
      </ListItem>

      <Divider variant="inset" component="li" />
    </>
  );
};

export default SliderField;
