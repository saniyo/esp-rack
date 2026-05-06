import React, { useEffect, useRef, useState } from 'react';
import {
  Divider,
  ListItem,
  ListItemAvatar,
  ListItemText,
  TextField as MuiTextField,
  Tooltip
} from '@mui/material';
import { styled } from '@mui/material/styles';
import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';
import { FieldAvatar } from '../utils/iconRegistry';

interface IPAddressFieldProps {
  field: Field;
  value: string;
  onChange: (label: string, value: string) => void;
  onBlur?: (label: string, value: string) => void;
}

// IPv4 dotted-quad: four octets 0..255 separated by dots.
const IPV4_REGEX = /^(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){3}$/;

const StyledTextField = styled(MuiTextField)<{ isReadOnly: boolean }>(({ isReadOnly, theme }) => ({
  cursor: isReadOnly ? 'not-allowed' : 'text',
  pointerEvents: isReadOnly ? 'none' : 'auto',
  '&.Mui-disabled': {
    backgroundColor: theme.palette.action.disabledBackground,
  },
}));

const IPAddressField: React.FC<IPAddressFieldProps> = ({ field, value, onChange, onBlur }) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');
  const isReadOnly = !!optionMap.r;
  const colorName = (optionMap as any).color?.value as string | undefined;

  const [internalValue, setInternalValue] = useState(value ?? '');
  const [isEditing, setIsEditing] = useState(false);
  const inputRef = useRef<HTMLInputElement>(null);

  useEffect(() => {
    if (!isEditing) setInternalValue(value ?? '');
  }, [value, isEditing]);

  const isValid = !internalValue || IPV4_REGEX.test(internalValue);
  const helperText = !isValid ? 'Invalid IPv4 address' : undefined;

  const commit = () => {
    setIsEditing(false);
    if (!IPV4_REGEX.test(internalValue)) return;  // block propagation on invalid
    onBlur?.(field.label, internalValue);
    onChange(field.label, internalValue);
  };

  const handleKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      commit();
      inputRef.current?.blur();
    }
  };

  if (isReadOnly) {
    return (
      <>
        <ListItem>
          <ListItemAvatar>
            <FieldAvatar iconName={field.icon} colorName={colorName} />
          </ListItemAvatar>
          <ListItemText primary={readableLabel} secondary={value || '—'} />
        </ListItem>
        <Divider variant="inset" component="li" />
      </>
    );
  }

  return (
    <ListItem>
      <Tooltip title={field.placeholder || 'Enter IPv4 address (e.g. 192.168.4.1)'}>
        <StyledTextField
          label={readableLabel}
          placeholder={field.placeholder || '0.0.0.0'}
          value={internalValue}
          onChange={(e) => setInternalValue(e.target.value)}
          onFocus={() => setIsEditing(true)}
          onBlur={commit}
          onKeyDown={handleKeyDown}
          inputRef={inputRef}
          fullWidth
          error={!isValid}
          helperText={helperText}
          isReadOnly={false}
        />
      </Tooltip>
    </ListItem>
  );
};

export default IPAddressField;
