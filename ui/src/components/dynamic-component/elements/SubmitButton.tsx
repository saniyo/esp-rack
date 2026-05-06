import React from 'react';
import { Button } from '@mui/material';

interface SubmitButtonProps {
  onSubmit: (event: React.FormEvent) => void;
  saving: boolean;
}

const SubmitButton: React.FC<SubmitButtonProps> = ({ onSubmit, saving }) => {
  return (
    <Button
      type="submit"
      variant="contained"
      color="primary"
      onClick={(e) => {
        console.log('Submit button clicked');
        onSubmit(e);
      }}
      disabled={saving}
      style={{ marginTop: 16, marginBottom: 8 }}
    >
      Save
    </Button>
  );
};

export default SubmitButton;
