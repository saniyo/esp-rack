// Switch.tsx
import React, { useEffect, useState, useRef } from 'react';
import { Switch as MuiSwitch, FormControlLabel, ListItem, Tooltip } from '@mui/material';
import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';

interface SwitchProps {
  field: Field;
  value: any;
  onChange: (label: string, value: any) => void;
  onBlur: (label: string, value: any) => void;
}

const Switch: React.FC<SwitchProps> = ({ field, value, onChange, onBlur }) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');
  const isReadOnly = !!optionMap.r;

  const [localValue, setLocalValue] = useState<boolean>(Boolean(value));
  const isInteracting = useRef(false);

  useEffect(() => {
    if (!isInteracting.current) {
      setLocalValue(Boolean(value));
    }
  }, [value]);

  const handleClick = () => {
    if (isReadOnly) return;
    const newChecked = !localValue;
    setLocalValue(newChecked);
    isInteracting.current = true;
    onChange(field.label, newChecked); // тільки boolean
    onBlur(field.label, newChecked);   // тільки boolean
  };

  const handleMouseLeave = () => {
    isInteracting.current = false;
  };

  return (
    <ListItem onMouseLeave={handleMouseLeave}>
      <Tooltip title={isReadOnly ? 'Read-only field' : 'Click to toggle value'}>
        <FormControlLabel
          control={
            <MuiSwitch
              checked={localValue}
              onClick={handleClick}
              disabled={isReadOnly}
            />
          }
          label={readableLabel}
        />
      </Tooltip>
    </ListItem>
  );
};

export default Switch;
