// interface/src/components/dynamic-component/elements/StatusItem.tsx
import React from 'react';
import { ListItem, ListItemIcon, ListItemText } from '@mui/material';
import { CheckCircleOutline, HighlightOff } from '@mui/icons-material';
import { Field } from '../types';

const StatusItem: React.FC<{ field: Field; value: any }> = ({ field, value }) => (
  <ListItem>
    <ListItemIcon>
      {value ? <CheckCircleOutline color="primary" /> : <HighlightOff color="error" />}
    </ListItemIcon>
    <ListItemText primary={field.label} secondary={value} />
  </ListItem>
);

export default StatusItem;
