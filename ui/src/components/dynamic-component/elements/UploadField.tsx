import React, { FC, useCallback, useState } from 'react';
import axios from 'axios';
import { Box, Typography } from '@mui/material';

import SingleUpload, { UploadThemeColor } from '../../upload/SingleUpload';
import { AXIOS } from '../../../api/endpoints';
import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';

interface UploadFieldProps {
  field: Field;
}

const UPLOAD_COLORS: readonly UploadThemeColor[] = [
  'primary', 'secondary', 'error', 'warning', 'success', 'info',
];

const UploadField: FC<UploadFieldProps> = ({ field }) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');
  const url = ((optionMap as any).url?.value as string) || '';
  const accept = ((optionMap as any).accept?.value as string) || undefined;
  const colorRaw = ((optionMap as any).color?.value as string | undefined);
  const themeColor: UploadThemeColor | undefined =
    colorRaw && (UPLOAD_COLORS as readonly string[]).includes(colorRaw)
      ? (colorRaw as UploadThemeColor)
      : undefined;

  const [uploading, setUploading] = useState(false);
  const [progress, setProgress] = useState<ProgressEvent>();
  const [error, setError] = useState<string | null>(null);
  const [cancelSource, setCancelSource] = useState<ReturnType<typeof axios.CancelToken.source>>();

  const onDrop = useCallback(
    async (accepted: File[]) => {
      if (!accepted.length || !url) return;
      const src = axios.CancelToken.source();
      setCancelSource(src);
      setUploading(true);
      setProgress(undefined);
      setError(null);
      try {
        const form = new FormData();
        form.append('file', accepted[0], accepted[0].name);
        await AXIOS.post(url, form, {
          headers: { 'Content-Type': 'multipart/form-data' },
          onUploadProgress: (ev: ProgressEvent) => setProgress(ev),
          cancelToken: src.token,
        });
      } catch (e: any) {
        if (!axios.isCancel(e)) {
          setError(e?.response?.data?.error || e?.message || 'upload_failed');
        }
      } finally {
        setUploading(false);
        setCancelSource(undefined);
      }
    },
    [url]
  );

  return (
    <Box sx={{ width: '100%', my: 2, px: 2 }}>
      <Typography variant="subtitle1" gutterBottom>
        {readableLabel}
      </Typography>
      <SingleUpload
        onDrop={onDrop}
        onCancel={() => cancelSource?.cancel()}
        uploading={uploading}
        progress={progress}
        accept={accept}
        themeColor={themeColor}
      />
      {error && (
        <Typography color="error" variant="body2" sx={{ mt: 1 }}>
          {error}
        </Typography>
      )}
    </Box>
  );
};

export default UploadField;
