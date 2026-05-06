import React, { useEffect, useState } from 'react';
import {
  FormControl,
  FormControlLabel,
  RadioGroup,
  Radio,
  ListItem,
  Tooltip,
  ListItemAvatar,
  ListItemText,
  Divider,
  FormLabel
} from '@mui/material';
import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';
import { FieldAvatar } from '../utils/iconRegistry';

interface RadioFieldProps {
  field: Field;
  value: number;
  onChange: (label: string, value: number) => void;
  onBlur?: (label: string, value: number) => void;
}

interface RadioOpt {
  label: string;
  value: number;
}

const RadioField: React.FC<RadioFieldProps> = ({ field, value, onChange, onBlur }) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');

  const isReadOnly = !!optionMap.r;

  // Mirror the Dropdown's mixed encoding: "Label:value" when present,
  // otherwise fall back to a 1-based index derived from position.
  const parsedOptions: RadioOpt[] = React.useMemo(() => {
    const optionsVal = optionMap.options?.value;
    const raw = Array.isArray(optionsVal) ? (optionsVal as string[]) : [];
    return raw.map((entry, idx) => {
      const colonIdx = entry.lastIndexOf(':');
      if (colonIdx > 0) {
        const label = entry.substring(0, colonIdx);
        const n = parseInt(entry.substring(colonIdx + 1), 10);
        return { label, value: Number.isFinite(n) ? n : idx + 1 };
      }
      return { label: entry, value: idx + 1 };
    });
  }, [optionMap]);

  const findLabelForValue = (v: any): string => {
    const n = Number(v);
    return parsedOptions.find((o) => o.value === n)?.label || '';
  };

  const [internalLabel, setInternalLabel] = useState<string>(findLabelForValue(value));

  useEffect(() => {
    setInternalLabel(findLabelForValue(value));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [value, parsedOptions]);

  const handleChange = (event: React.ChangeEvent<HTMLInputElement>) => {
    const selectedLabel = event.target.value;
    const opt = parsedOptions.find((o) => o.label === selectedLabel);
    if (!opt) return;
    setInternalLabel(selectedLabel);
    onChange(field.label, opt.value);
    onBlur?.(field.label, opt.value);
  };

  const hint = `${Object.keys(optionMap).join(', ')}` || 'This is a radio field';
  const colorName = (optionMap as any).color?.value as string | undefined;

  if (isReadOnly) {
    return (
      <>
        <ListItem>
          <ListItemAvatar>
            <FieldAvatar iconName={field.icon} colorName={colorName} />
          </ListItemAvatar>
          <ListItemText primary={readableLabel} secondary={internalLabel} />
        </ListItem>
        <Divider variant="inset" component="li" />
      </>
    );
  }

  return (
    <ListItem>
      <FormControl component="fieldset">
        <FormLabel component="legend">{readableLabel}</FormLabel>
        <Tooltip title={hint}>
          <RadioGroup value={internalLabel} onChange={handleChange}>
            {parsedOptions.map((opt) => (
              <FormControlLabel
                key={opt.value}
                value={opt.label}
                control={<Radio />}
                label={opt.label}
              />
            ))}
          </RadioGroup>
        </Tooltip>
      </FormControl>
    </ListItem>
  );
};

export default RadioField;
