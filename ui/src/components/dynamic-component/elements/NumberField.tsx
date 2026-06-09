// NumberField.tsx (покращений)
import React, { useState, useEffect } from 'react';
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

interface NumberFieldProps {
  field: Field;
  value: string; // значення, яке приходить зверху
  onChange: (label: string, value: string) => void;
  onBlur?: (label: string, value: string) => void;
}

const NumberField: React.FC<NumberFieldProps> = ({
  field,
  value,
  onBlur,
  onChange,
}) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');
  const isReadOnly = !!optionMap.r;
  const minValue = optionMap.mn?.value;
  const maxValue = optionMap.mx?.value;

  // Локальний стан (якщо треба редагувати «на льоту»)
  const [internalValue, setInternalValue] = useState(value);
  const [isEditing, setIsEditing] = useState(false);

  // Якщо зовнішнє value змінилося (зверху), оновлюємо локальний стан,
  // але лише якщо користувач зараз не редагує
  useEffect(() => {
    if (!isEditing) {
      setInternalValue(value);
    }
  }, [value, isEditing]);

  // Free typing: store the raw input verbatim. We must NOT clamp here, on
  // every keystroke — that makes intermediate values impossible to type: to
  // enter "15" when min=10 the transient "1" would snap to "10" and you could
  // never reach 15, and clearing the field would snap straight to min. Bounds
  // are applied once, at commit (blur), in clampToBounds().
  const handleInputChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    setInternalValue(e.target.value);
  };

  // Clamp to [min, max] — only at commit time. Empty / non-numeric is passed
  // through untouched (the form keeps the last good value).
  const clampToBounds = (raw: string): string => {
    if (raw === '') return raw;
    const n = parseFloat(raw);
    if (Number.isNaN(n)) return raw;
    let c = n;
    if (minValue !== undefined && c < +minValue) c = +minValue;
    if (maxValue !== undefined && c > +maxValue) c = +maxValue;
    return String(c);
  };

  // Коли користувач втрачає фокус
  const handleBlur = () => {
    setIsEditing(false);
    // Apply min/max ONCE, here at commit — not per keystroke (see
    // handleInputChange). Reflect the clamped value back into the input and
    // notify the parent with the final value.
    const finalValue = clampToBounds(internalValue);
    if (finalValue !== internalValue) setInternalValue(finalValue);
    if (onBlur) onBlur(field.label, finalValue);
    if (onChange) onChange(field.label, finalValue);
  };

  // Коли поле отримує фокус
  const handleFocus = () => {
    setIsEditing(true);
  };

  // Якщо користувач натискає Enter
  const handleKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      handleBlur();
    }
  };

  if (isReadOnly) {
    const unit = (optionMap as any).unit?.value as string | undefined;
    const displayValue = unit ? `${value} ${unit}` : value;
    const colorName = (optionMap as any).color?.value as string | undefined;
    return (
      <>
        <ListItem>
          <ListItemAvatar>
            <FieldAvatar iconName={field.icon} colorName={colorName} />
          </ListItemAvatar>
          <ListItemText primary={readableLabel} secondary={displayValue} />
        </ListItem>
        <Divider variant="inset" component="li" />
      </>
    );
  }

  return (
    <ListItem>
      <Tooltip title={field.placeholder || 'Enter a value'}>
        <StyledTextField
          type="number"
          label={readableLabel}
          placeholder={field.placeholder || ''}
          value={internalValue}
          onChange={handleInputChange}
          onFocus={handleFocus}
          onBlur={handleBlur}
          onKeyDown={handleKeyDown}
          fullWidth
        />
      </Tooltip>
    </ListItem>
  );
};

const StyledTextField = styled(MuiTextField)(() => ({
  // за потреби кастомні стилі
}));

export default NumberField;
