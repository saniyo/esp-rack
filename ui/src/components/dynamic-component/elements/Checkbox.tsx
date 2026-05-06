import React, { useEffect, useState, useRef } from 'react';
import { Checkbox as MuiCheckbox, FormControlLabel, ListItem, Tooltip } from '@mui/material';
import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';

interface CheckboxProps {
  field: Field;
  value: any;
  onChange: (label: string, value: any) => void;
  onBlur: (label: string, value: any) => void;
}

const Checkbox: React.FC<CheckboxProps> = ({ field, value, onChange, onBlur }) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');
  const isReadOnly = !!optionMap.r;

  const [localValue, setLocalValue] = useState<boolean>(!!value);
  const isInteracting = useRef(false);

  useEffect(() => {
    if (!isInteracting.current) {
      setLocalValue(!!value);
    }
  }, [value]);

  const handleChange: React.ChangeEventHandler<HTMLInputElement> = (e) => {
    if (isReadOnly) return;
    const checked = e.target.checked;
    setLocalValue(checked);
    isInteracting.current = true;
    onChange(field.label, checked);
    onBlur(field.label, checked);
  };

  const handleMouseLeave = () => {
    isInteracting.current = false;
  };

  return (
    <ListItem onMouseLeave={handleMouseLeave}>
      <Tooltip title={isReadOnly ? 'Read-only field' : 'Click to toggle value'}>
        <FormControlLabel
          control={
            <MuiCheckbox
              checked={localValue}
              onChange={handleChange}
              disabled={isReadOnly}
              inputProps={{ 'aria-label': readableLabel }}
            />
          }
          label={readableLabel}
        />
      </Tooltip>
    </ListItem>
  );
};

export default Checkbox;
