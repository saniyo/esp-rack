// ButtonField.tsx
import React, { useEffect, useRef, useState } from 'react';
import { ListItem, Tooltip, Button } from '@mui/material';
import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';

interface ButtonFieldProps {
  field: Field;
  value: any; // очікуємо boolean
  onChange: (label: string, value: any) => void;
  onBlur: (label: string, value: any) => void;
}

const ButtonField: React.FC<ButtonFieldProps> = ({ field, value, onChange, onBlur }) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');
  const isReadOnly = !!optionMap.r;

  const [localOn, setLocalOn] = useState<boolean>(Boolean(value));
  const interactingRef = useRef(false);

  useEffect(() => {
    const ext = Boolean(value);
    if (!interactingRef.current) {
      setLocalOn(ext);
    } else if (ext === localOn) {
      interactingRef.current = false;
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [value]);

  const labelText = field.placeholder || readableLabel || '';
  const tooltipText =
    optionMap.pl?.value?.toString() || (isReadOnly ? 'Read-only action' : 'Click to toggle');

  const handleClick = () => {
    if (isReadOnly) return;
    const next = !localOn;
    setLocalOn(next);
    interactingRef.current = true;

    onChange(field.label, next); // тільки boolean
    onBlur(field.label, next);   // тільки boolean
  };

  const handleKeyDown: React.KeyboardEventHandler<HTMLButtonElement> = (e) => {
    if (isReadOnly) return;
    if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      handleClick();
    }
  };

  return (
    <ListItem>
      <Tooltip title={tooltipText}>
          <Button
            variant={localOn ? 'contained' : 'outlined'}
            color={localOn ? 'primary' : 'inherit'}
            onClick={handleClick}
            onKeyDown={handleKeyDown}
            disabled={isReadOnly}
          >
            {labelText}
          </Button>
      </Tooltip>
    </ListItem>
  );
};

export default ButtonField;
