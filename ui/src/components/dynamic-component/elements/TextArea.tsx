import React, { useState, useEffect } from 'react';
import {
  TextField as MuiTextField,
  Tooltip,
  ListItem,
  ListItemAvatar,
  ListItemText,
  Divider
} from '@mui/material';
import { Field } from '../types';
import { styled } from '@mui/material/styles';
import { useFieldParser } from '../utils/useFieldParser';
import { FieldAvatar } from '../utils/iconRegistry';

interface TextareaProps {
  field: Field;
  value: string;
  onChange: (label: string, value: string) => void;
  onBlur?: (label: string, value: string) => void;
}

// Стилізований компонент для текстового поля у режимі тільки для читання
const StyledReadOnlyTextField = styled(MuiTextField)(({ theme }) => ({
  '& .MuiInputBase-root.Mui-disabled': {
    backgroundColor: theme.palette.action.disabledBackground,
  },
}));

const Textarea: React.FC<TextareaProps> = ({ field, value, onChange, onBlur }) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');
  const isReadOnly = !!optionMap.r;

  const [internalValue, setInternalValue] = useState<string>(value);
  const [isEditing, setIsEditing] = useState(false);

  useEffect(() => {
    if (!isEditing) {
      setInternalValue(value);
    }
  }, [value, isEditing]);

  const handleInputChange = (e: React.ChangeEvent<HTMLInputElement | HTMLTextAreaElement>) => {
    const newValue = e.target.value;
    setInternalValue(newValue);
    onChange(field.label, newValue); // Відправляємо зміну під час введення
  };

  const handleBlur = () => {
    setIsEditing(false);
    onBlur?.(field.label, internalValue); // Відправляємо зміну при втраті фокусу
  };

  const handleFocus = () => {
    setIsEditing(true);
  };

  const hint = field.placeholder || 'Enter text';

  if (isReadOnly) {
    return (
      <>
        <ListItem>
          <ListItemAvatar>
            <FieldAvatar iconName={field.icon} colorName={(optionMap as any).color?.value as string | undefined} />
          </ListItemAvatar>
          <ListItemText
            primary={readableLabel}
          />
        </ListItem>
        <ListItem>
          <StyledReadOnlyTextField
            variant="outlined"
            multiline
            minRows={3}
            fullWidth
            value={internalValue}
            disabled
            InputProps={{
              readOnly: true,
            }}
          />
        </ListItem>
        <Divider variant="inset" component="li" />
      </>
    );
  }

  return (
    <ListItem>
      <Tooltip title={hint}>
        <MuiTextField
          label={readableLabel}
          placeholder={field.placeholder || ''}
          value={internalValue}
          onChange={handleInputChange}
          onBlur={handleBlur}
          onFocus={handleFocus}
          multiline
          minRows={3}
          fullWidth
        />
      </Tooltip>
    </ListItem>
  );
};

export default Textarea;
