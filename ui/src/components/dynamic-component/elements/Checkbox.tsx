import React, { useCallback, useEffect, useState, useRef } from 'react';
import { Checkbox as MuiCheckbox, FormControlLabel, ListItem, Tooltip } from '@mui/material';
import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';

// After a click the clicked value is held this long (a live frame from before
// the save must not flip it back), then the device's value shows again: a save
// that failed or was refused doesn't leave the clicked value up (a phone has
// no mouse-leave to end the hold).
const HOLD_MS = 2000;

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
  const latest = useRef(value);
  latest.current = value;
  const holdTimer = useRef<number | undefined>(undefined);

  useEffect(() => {
    if (!isInteracting.current) {
      setLocalValue(!!value);
    }
  }, [value]);

  const release = useCallback(() => {
    window.clearTimeout(holdTimer.current);
    isInteracting.current = false;
    setLocalValue(!!latest.current);
  }, []);
  useEffect(() => () => window.clearTimeout(holdTimer.current), []);

  const handleChange: React.ChangeEventHandler<HTMLInputElement> = (e) => {
    if (isReadOnly) return;
    const checked = e.target.checked;
    setLocalValue(checked);
    isInteracting.current = true;
    window.clearTimeout(holdTimer.current);
    holdTimer.current = window.setTimeout(release, HOLD_MS);
    onChange(field.label, checked);
    onBlur(field.label, checked);
  };

  return (
    <ListItem>
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
