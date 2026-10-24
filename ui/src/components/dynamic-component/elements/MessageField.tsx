import React, { FC, useMemo } from 'react';
import { Alert, AlertTitle, Box } from '@mui/material';

import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';
import { resolveIcon } from '../utils/iconRegistry';

interface MessageFieldProps {
  field: Field;
  value: string | undefined;  // body text
  // levelKey: live severity from a sibling state value.
  getSibling?: (key: string) => any;
}

type AlertSeverity = 'info' | 'warning' | 'error' | 'success';

const SEVERITIES: readonly string[] = ['info', 'warning', 'error', 'success'];

const MessageField: FC<MessageFieldProps> = ({ field, value, getSibling }) => {
  const { readableLabel, labelOverridden, optionMap } = useFieldParser(field.label, field.o || '');

  // levelKey's live value wins when it's a severity; else the static level.
  const levelKey = (optionMap as any).levelKey?.value as string | undefined;
  const live = levelKey ? getSibling?.(levelKey) : undefined;
  const severity = useMemo<AlertSeverity>(() => {
    if (typeof live === 'string' && SEVERITIES.includes(live)) return live as AlertSeverity;
    const raw = ((optionMap as any).level?.value as string) || 'info';
    if (raw === 'warning' || raw === 'error' || raw === 'success') return raw;
    return 'info';
  }, [optionMap, live]);

  const IconComp = resolveIcon(field.icon);

  return (
    <Box sx={{ px: 2, py: 1 }}>
      <Alert
        severity={severity}
        icon={IconComp ? <IconComp fontSize="inherit" /> : undefined}
        variant="filled"
      >
        {/* A title only when the backend gave one (label(...)): the key of a
            message ("m_hw") is not a heading. */}
        {labelOverridden && readableLabel && <AlertTitle>{readableLabel}</AlertTitle>}
        {value ?? ''}
      </Alert>
    </Box>
  );
};

export default MessageField;
