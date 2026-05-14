import { FC } from 'react';
import {
  Box,
  Chip,
  Divider,
  ListItem,
  ListItemAvatar,
  ListItemText,
  Typography,
} from '@mui/material';
import ErrorOutlineIcon from '@mui/icons-material/ErrorOutline';

interface EndpointMissingProps {
  label: string;        // human-visible label of the field
  endpoint: string;     // URL prefix it would have hit
  reason: string;       // why it's unavailable (which backend module to install)
}

// Shown by dynamic widgets (FilesField, UploadField, …) when the
// REST endpoint they depend on isn't registered on the device. The
// field still occupies its slot in the form list — so the operator
// sees WHAT widget was supposed to live here and WHICH backend
// module would expose it — but skips the doomed fetch that would
// otherwise either spinner-forever or 404-snackbar-spam.
const EndpointMissing: FC<EndpointMissingProps> = ({ label, endpoint, reason }) => (
  <>
    <ListItem alignItems="flex-start">
      <ListItemAvatar>
        <Box sx={{ color: 'error.main', display: 'flex', alignItems: 'center', height: '100%' }}>
          <ErrorOutlineIcon fontSize="large" />
        </Box>
      </ListItemAvatar>
      <ListItemText
        primary={
          <Typography variant="subtitle1" color="error">
            {label} — endpoint unavailable
          </Typography>
        }
        secondary={
          <Box component="span" sx={{ display: 'block' }}>
            <Typography variant="body2" color="text.secondary" component="span" display="block">
              {reason}
            </Typography>
            <Chip
              label={endpoint}
              size="small"
              color="error"
              variant="outlined"
              sx={{ mt: 0.5, fontFamily: 'monospace' }}
            />
          </Box>
        }
      />
    </ListItem>
    <Divider variant="inset" component="li" />
  </>
);

export default EndpointMissing;
