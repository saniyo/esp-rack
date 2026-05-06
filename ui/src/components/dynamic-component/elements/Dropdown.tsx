import React, { useEffect, useState } from 'react';
import {
  FormControl,
  InputLabel,
  MenuItem,
  Select,
  ListItem,
  Tooltip,
  ListItemAvatar,
  ListItemText,
  Divider,
  SelectChangeEvent
} from '@mui/material';
import { Field } from '../types';
import { styled } from '@mui/material/styles';
import { useFieldParser } from '../utils/useFieldParser';
import { FieldAvatar } from '../utils/iconRegistry';

interface DropdownProps {
  field: Field;
  value: string | number;
  onChange: (label: string, value: number) => void;
  onBlur?: (label: string, value: number) => void;
}

// Стилізований компонент для Select з використанням transient props ($isReadOnly)
const StyledSelect = styled((props: any) => <Select {...props} />)<{ $isReadOnly: boolean }>(({ $isReadOnly }) => ({
  cursor: $isReadOnly ? 'not-allowed' : 'pointer',
  pointerEvents: $isReadOnly ? 'none' : 'auto',
  '& .MuiOutlinedInput-notchedOutline': {
    borderColor: $isReadOnly ? 'transparent' : undefined,
  },
}));

interface DropdownOpt {
  label: string;
  value: number;
}

const Dropdown: React.FC<DropdownProps> = ({ field, value, onChange, onBlur }) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');

  const isReadOnly = !!optionMap.r;

  // Backend may emit either:
  //   options=Label1,Label2,Label3        (legacy; value = 1-based index)
  //   options=Label1:0,Label2:1,Label3:2  (explicit ints per label)
  // Prefer explicit values when present, falling back to 1-based index otherwise.
  const parsedOptions: DropdownOpt[] = React.useMemo(() => {
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

  const handleChange = (event: SelectChangeEvent<string>) => {
    const selectedLabel = event.target.value;
    const opt = parsedOptions.find((o) => o.label === selectedLabel);
    if (!opt) return;
    setInternalLabel(selectedLabel);
    onChange(field.label, opt.value);
    onBlur?.(field.label, opt.value);
  };

  const hint = `${Object.keys(optionMap).join(', ')}` || 'This is a dropdown field';
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
      <Tooltip title={hint}>
        <FormControl fullWidth variant="outlined">
          <InputLabel>{readableLabel}</InputLabel>
          <StyledSelect
            value={internalLabel}
            onChange={handleChange}
            label={readableLabel}
            $isReadOnly={isReadOnly}
          >
            {parsedOptions.map((opt) => (
              <MenuItem key={opt.value} value={opt.label}>
                {opt.label}
              </MenuItem>
            ))}
          </StyledSelect>
        </FormControl>
      </Tooltip>
    </ListItem>
  );
};

export default Dropdown;
