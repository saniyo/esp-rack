import React, { useState, useEffect, useRef } from 'react';
import {
  ListItem,
  TextField as MuiTextField,
  Tooltip,
  Divider,
  ListItemAvatar,
  ListItemText,
  IconButton,
  InputAdornment,
} from '@mui/material';
import VisibilityIcon from '@mui/icons-material/Visibility';
import VisibilityOffIcon from '@mui/icons-material/VisibilityOff';
import { Field } from '../types';
import { styled } from '@mui/material/styles';
import { useFieldParser } from '../utils/useFieldParser';
import { FieldAvatar } from '../utils/iconRegistry';

interface SecretFieldProps {
  field: Field;
  value: string;
  onChange: (label: string, value: string) => void;
  onBlur?: (label: string, value: string) => void;
}

const StyledSecretField = styled(MuiTextField)<{ isReadOnly: boolean }>(({ isReadOnly, theme }) => ({
  cursor: isReadOnly ? 'not-allowed' : 'text',
  pointerEvents: isReadOnly ? 'none' : 'auto',
  '&.Mui-disabled': {
    backgroundColor: theme.palette.action.disabledBackground,
  },
}));

// Dedicated element for FieldType::SECRET — password / token / signing
// key. Renders type=password by default with an eye-toggle, and never
// echoes the value into the floating label or read-only secondary.
// Frontend mirrors the trust model of ValidatedPasswordField but lives
// inside the dynamic-component pipeline (manifest-driven forms) so the
// same affordance is available across every dynamic settings tab.
const SecretField: React.FC<SecretFieldProps> = ({ field, value, onChange, onBlur }) => {
  const { readableLabel, optionMap, labelOverridden } = useFieldParser(field.label, field.o || '');
  const [internalValue, setInternalValue] = useState(value);
  const [isEditing, setIsEditing] = useState(false);
  const [showSecret, setShowSecret] = useState(false);
  const inputRef = useRef<HTMLInputElement>(null);

  const isReadOnly = !!optionMap.r;
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

  // Read-only path renders a masked secondary so the value never
  // appears in plaintext in the UI even for screen-readers / DevTools.
  if (isReadOnly) {
    const hideAvatar = !!((optionMap as any).hideAvatar?.value);
    return (
      <>
        <ListItem>
          {!hideAvatar && (
            <ListItemAvatar>
              <FieldAvatar iconName={field.icon} colorName={undefined} />
            </ListItemAvatar>
          )}
          <Tooltip title={keyHint} placement="right" disableHoverListener={!keyHint}>
            <ListItemText primary={readableLabel} secondary={value ? '••••••••' : ''} />
          </Tooltip>
        </ListItem>
        <Divider variant="inset" component="li" />
      </>
    );
  }

  const labelShrink = !!(internalValue && String(internalValue).length > 0) || isEditing;

  return (
    <ListItem>
      <Tooltip title={keyHint || field.placeholder || 'Enter secret'}>
        <StyledSecretField
          type={showSecret ? 'text' : 'password'}
          label={readableLabel}
          placeholder={field.placeholder || ''}
          value={internalValue}
          onChange={handleInputChange}
          onBlur={handleBlur}
          onFocus={handleFocus}
          onKeyDown={handleKeyDown}
          inputRef={inputRef}
          fullWidth
          autoComplete="new-password"
          InputProps={{
            readOnly: isReadOnly,
            endAdornment: (
              <InputAdornment position="end">
                <IconButton
                  aria-label="toggle secret visibility"
                  onClick={() => setShowSecret((s) => !s)}
                  edge="end"
                  size="small"
                >
                  {showSecret ? <VisibilityIcon /> : <VisibilityOffIcon />}
                </IconButton>
              </InputAdornment>
            ),
          }}
          InputLabelProps={{ shrink: labelShrink }}
          disabled={isReadOnly}
          isReadOnly={isReadOnly}
        />
      </Tooltip>
    </ListItem>
  );
};

export default SecretField;
