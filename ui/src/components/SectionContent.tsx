import React from 'react';

import { Box, Paper, Typography } from '@mui/material';

import { RequiredChildrenProps } from '../utils';

interface SectionContentProps extends RequiredChildrenProps {
  title: string;
  titleGutter?: boolean;
  // Optional right-aligned slot rendered on the same row as the title
  // (e.g. tab reload button + live indicator). Keeps form chrome
  // visually balanced against the heading instead of stacking below.
  actions?: React.ReactNode;
}

const SectionContent: React.FC<SectionContentProps> = (props) => {
  const { children, title, titleGutter, actions } = props;
  return (
    <Paper sx={{ p: 2, m: 2 }}>
      <Box
        sx={{
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'space-between',
          gap: 1,
          mb: titleGutter ? 1 : 0,
        }}
      >
        <Typography variant="h6">{title}</Typography>
        {actions && <Box sx={{ display: 'flex', alignItems: 'center', gap: 1 }}>{actions}</Box>}
      </Box>
      {children}
    </Paper>
  );
};

export default SectionContent;
