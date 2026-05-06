import React, { FC, useMemo } from 'react';
import { Alert, AlertTitle, Box } from '@mui/material';

import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';
import { resolveIcon } from '../utils/iconRegistry';

interface MessageFieldProps {
  field: Field;
  value: string | undefined;  // body text
}

type AlertSeverity = 'info' | 'warning' | 'error' | 'success';

const MessageField: FC<MessageFieldProps> = ({ field, value }) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');

  const severity = useMemo<AlertSeverity>(() => {
    const raw = ((optionMap as any).level?.value as string) || 'info';
    if (raw === 'warning' || raw === 'error' || raw === 'success') return raw;
    return 'info';
  }, [optionMap]);

  const IconComp = resolveIcon(field.icon);

  return (
    <Box sx={{ px: 2, py: 1 }}>
      <Alert
        severity={severity}
        icon={IconComp ? <IconComp fontSize="inherit" /> : undefined}
        variant="filled"
      >
        {readableLabel && readableLabel !== field.label && (
          <AlertTitle>{readableLabel}</AlertTitle>
        )}
        {value ?? ''}
      </Alert>
    </Box>
  );
};

export default MessageField;
