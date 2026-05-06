import { FC, Fragment } from 'react';
import { useDropzone, DropzoneState } from 'react-dropzone';

import { Box, Button, LinearProgress, Theme, Typography, alpha, useTheme } from '@mui/material';

import CloudUploadIcon from '@mui/icons-material/CloudUpload';
import CancelIcon from '@mui/icons-material/Cancel';

const progressPercentage = (progress: ProgressEvent) => Math.round((progress.loaded * 100) / progress.total);

export type UploadThemeColor =
  | 'primary'
  | 'secondary'
  | 'error'
  | 'warning'
  | 'success'
  | 'info';

const palettePick = (theme: Theme, c?: UploadThemeColor): string | undefined => {
  if (!c) return undefined;
  const p = (theme.palette as any)[c];
  return p && p.main ? p.main : undefined;
};

const getBorderColor = (theme: Theme, props: DropzoneState, themeColor?: UploadThemeColor) => {
  if (props.isDragAccept) return theme.palette.success.main;
  if (props.isDragReject) return theme.palette.error.main;
  if (props.isDragActive) return theme.palette.info.main;
  return palettePick(theme, themeColor) ?? theme.palette.grey[700];
};

// Background tint is applied only DURING a drag so the idle zone stays on
// the page background. themeColor affects the border only — the zone
// doesn't compete with a filled sibling Alert that might already fill
// a row above it.
const getBackgroundColor = (_theme: Theme, props: DropzoneState, _themeColor?: UploadThemeColor) => {
  if (props.isDragAccept) return alpha(_theme.palette.success.main, 0.22);
  if (props.isDragReject) return alpha(_theme.palette.error.main, 0.22);
  if (props.isDragActive) return alpha(_theme.palette.info.main, 0.22);
  return 'transparent';
};

export interface SingleUploadProps {
  onDrop: (acceptedFiles: File[]) => void;
  onCancel: () => void;
  accept?: string | string[];
  uploading: boolean;
  progress?: ProgressEvent;
  themeColor?: UploadThemeColor;
}

const SingleUpload: FC<SingleUploadProps> = ({ onDrop, onCancel, accept, uploading, progress, themeColor }) => {
  const dropzoneState = useDropzone({ onDrop, accept, disabled: uploading, multiple: false });
  const { getRootProps, getInputProps } = dropzoneState;
  const theme = useTheme();

  const progressText = () => {
    if (uploading) {
      if (progress?.lengthComputable) {
        return `Uploading: ${progressPercentage(progress)}%`;
      }
      return "Uploading\u2026";
    }
    return "Drop file or click here";
  };

  return (
    <Box
      {...getRootProps({
        sx: {
          py: 8,
          px: 2,
          borderWidth: 2,
          borderRadius: 2,
          borderStyle: 'dashed',
          color: theme.palette.grey[700],
          transition: 'border .24s ease-in-out, background-color .24s ease-in-out',
          width: '100%',
          cursor: uploading ? 'default' : 'pointer',
          borderColor: getBorderColor(theme, dropzoneState, themeColor),
          backgroundColor: getBackgroundColor(theme, dropzoneState, themeColor)
        }
      })}
    >
      <input {...getInputProps()} />
      <Box flexDirection="column" display="flex" alignItems="center">
        <CloudUploadIcon fontSize='large' />
        <Typography variant="h6">
          {progressText()}
        </Typography>
        {uploading && (
          <Fragment>
            <Box width="100%" p={2}>
              <LinearProgress
                variant={!progress || progress.lengthComputable ? "determinate" : "indeterminate"}
                value={!progress ? 0 : progress.lengthComputable ? progressPercentage(progress) : 0}
              />
            </Box>
            <Button startIcon={<CancelIcon />} variant="contained" color="secondary" onClick={onCancel}>
              Cancel
            </Button>
          </Fragment>
        )}
      </Box>
    </Box >
  );
};

export default SingleUpload;
