import React, { useState, useEffect, useRef } from 'react';
import {
  ListItem,
  TextField as MuiTextField,
  Tooltip,
  Divider,
  ListItemAvatar,
  ListItemText,
} from '@mui/material';
import { Field } from '../types';
import { styled } from '@mui/material/styles';
import { useFieldParser } from '../utils/useFieldParser';
import { FieldAvatar } from '../utils/iconRegistry';

interface TextFieldProps {
  field: Field;
  value: string;
  onChange: (label: string, value: string) => void;
  onBlur?: (label: string, value: string) => void;
}

const StyledTextField = styled(MuiTextField)<{ isReadOnly: boolean }>(({ isReadOnly, theme }) => ({
  cursor: isReadOnly ? 'not-allowed' : 'text',
  pointerEvents: isReadOnly ? 'none' : 'auto',
  '&.Mui-disabled': {
    backgroundColor: theme.palette.action.disabledBackground,
  },
}));

const TextField: React.FC<TextFieldProps> = ({ field, value, onChange, onBlur }) => {
  const { readableLabel, optionMap, labelOverridden } = useFieldParser(field.label, field.o || '');
  const [internalValue, setInternalValue] = useState(value);
  const [isEditing, setIsEditing] = useState(false);
  const inputRef = useRef<HTMLInputElement>(null);

  const isReadOnly = !!optionMap.r;
  // Tooltip text — when backend overrode the label, show the underlying
  // JSON key on hover so the operator can map UI label ↔ wire/disk key.
  // Empty when no override (the auto-titlecased label IS the key).
  const keyHint = labelOverridden ? field.label : '';

  useEffect(() => {
    if (!isEditing) {
      setInternalValue(value);
    }
  }, [value, isEditing]);

  const handleInputChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    setInternalValue(e.target.value);
  };

  const handleBlur = () => {
    setIsEditing(false);
    onBlur?.(field.label, internalValue);
    onChange?.(field.label, internalValue);
  };

  const handleFocus = () => {
    setIsEditing(true);
  };

  const handleKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      onBlur?.(field.label, internalValue);
      onChange?.(field.label, internalValue);
      inputRef.current?.blur();
    }
  };

  const unit = (optionMap as any).unit?.value as string | undefined;

  const getLabel = () => (
    <>
      {readableLabel} {isEditing && `: ${value}`}
    </>
  );

  if (isReadOnly) {
    const displayValue = unit ? `${value} ${unit}` : value;

    // Live color: a `colorMap` tag maps the current rendered value to a
    // palette key (Connected→success, Connecting→warning, …). When the
    // value changes over WS the Avatar background follows. Falls back to
    // the static `color` tag, then to the default Avatar palette.
    const colorMapVal = (optionMap as any).colorMap?.value as Record<string, string> | undefined;
    const staticColor = (optionMap as any).color?.value as string | undefined;
    const valueKey = value === undefined || value === null ? '' : String(value);
    const mapped = colorMapVal ? (colorMapVal[valueKey] ?? colorMapVal['default']) : undefined;
    const effectiveColor = mapped ?? staticColor;
    const hideAvatar = !!((optionMap as any).hideAvatar?.value);

    return (
      <>
        <ListItem>
          {!hideAvatar && (
            <ListItemAvatar>
              <FieldAvatar iconName={field.icon} colorName={effectiveColor} />
            </ListItemAvatar>
          )}
          <Tooltip title={keyHint} placement="right" disableHoverListener={!keyHint}>
            <ListItemText primary={readableLabel} secondary={displayValue} />
          </Tooltip>
        </ListItem>
        <Divider variant="inset" component="li" />
      </>
    );
  }

  // Force the floating label into "shrunk" state whenever the input
  // has a value or is focused. MUI's auto-detect is supposed to do
  // this but loses sync when the value arrives via cross-tab prefill
  // (router state) — value=Silicone shows in the input while the
  // label sits across the top of it instead of pinning to the top
  // border. Explicit shrink fixes it.
  const labelShrink = !!(internalValue && String(internalValue).length > 0) || isEditing;

  return (
    <ListItem>
      <Tooltip title={keyHint || field.placeholder || 'Enter text'}>
        <StyledTextField
          type="text"
          label={getLabel()}
          placeholder={field.placeholder || ''}
          value={internalValue}
          onChange={handleInputChange}
          onBlur={handleBlur}
          onFocus={handleFocus}
          onKeyDown={handleKeyDown}
          inputRef={inputRef}
          fullWidth
          InputProps={{ readOnly: isReadOnly }}
          InputLabelProps={{ shrink: labelShrink }}
          disabled={isReadOnly}
          isReadOnly={isReadOnly}
        />
      </Tooltip>
    </ListItem>
  );
};

export default TextField;
