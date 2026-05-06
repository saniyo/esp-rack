import React, { FC, useEffect, useMemo, useState } from 'react';
import {
  Button,
  Dialog,
  DialogActions,
  DialogContent,
  DialogTitle,
  ListItem,
  Box,
} from '@mui/material';
import { useSnackbar } from 'notistack';

import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';
import { AXIOS } from '../../../api/endpoints';
import { extractErrorMessage } from '../../../utils';
import { resolveIcon } from '../utils/iconRegistry';
import { useManifest } from '../../../contexts/manifest';
import { useDynamicForm } from '../DynamicFormContext';
import { ShowIfSpec } from '../utils/fieldParser';

// Polling schedule (ms from click) driven when a field carries
// refetchForm=1. The first refetch is intentionally delayed past the
// radio's scan window (~2-3 s on ESP32) — hitting the device while it's
// hopping channels during a scan otherwise yields ERR_NETWORK_CHANGED
// which is useless noise. Three refetches cover the common 2-8 s scan
// duration; silent mode swallows transient failures.
const REFETCH_SCHEDULE_MS = [3000, 6000, 10000] as const;
const PENDING_WINDOW_MS = 12000;

interface ActionFieldProps {
  field: Field;
  value: string | undefined;  // button label
}

type MuiColor =
  | 'primary'
  | 'secondary'
  | 'error'
  | 'warning'
  | 'success'
  | 'info'
  | 'inherit';

const toAxiosPath = (absPath: string) =>
  absPath.startsWith('/rest/') ? absPath.slice(5) : absPath;

const isMuiColor = (s?: string): s is MuiColor =>
  !!s && ['primary', 'secondary', 'error', 'warning', 'success', 'info', 'inherit'].includes(s);

const ActionField: FC<ActionFieldProps> = ({ field, value }) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');
  const { enqueueSnackbar } = useSnackbar();
  const { findFeature } = useManifest();
  const { refetch, markPending, clearPending, isPending, formState } = useDynamicForm();

  const refetchAfter = !!((optionMap as any).refetchForm?.value);

  // When loadingIf is present, the button stays disabled while the
  // referenced form field matches the prefix (e.g. scan_status starting
  // with "Scanning"). The pending-timer provides a fallback upper bound
  // so the button doesn't stay stuck if the backend never flips the
  // state (crash, lost REST, …). Timer is cleared early as soon as the
  // form-state condition clears so the button un-busies immediately
  // when the backend reports "Found N".
  const loadingSpec = (optionMap as any).loadingIf?.value as ShowIfSpec | undefined;
  const actionId = useMemo(() => {
    const ref = (optionMap as any).ref?.value as string | undefined;
    return ref || field.label;
  }, [optionMap, field.label]);

  const busyFromState = !!(loadingSpec && loadingSpec.field &&
    typeof formState[loadingSpec.field] !== 'undefined' &&
    String(formState[loadingSpec.field]).startsWith(loadingSpec.val));

  // Release the fallback pending timer as soon as the backend indicates
  // completion so the button becomes active the instant scan_status
  // flips from "Scanning…" to "Found N".
  useEffect(() => {
    if (!busyFromState && isPending(actionId)) clearPending(actionId);
  }, [busyFromState, actionId, isPending, clearPending]);

  // Prefer a manifest-registered action when ref=<id> is set; the action
  // entry owns URL/method/confirm/color/icon/title/successMessage. Inline
  // url/method/confirm/color options still work as a fallback.
  const refId = ((optionMap as any).ref?.value as string) || '';
  const refEntry = refId ? findFeature(refId) : undefined;
  const usingRef = refEntry && refEntry.kind === 'action';

  const resolved = useMemo(() => {
    if (usingRef && refEntry) {
      const a: any = refEntry;
      return {
        url: (a.restPath as string) || '',
        method: ((a.method as string) || 'POST').toUpperCase(),
        confirmText: (a.confirm as string) || '',
        colorRaw: a.color as string | undefined,
        iconName: (a.icon as string) || field.icon,
        title: (a.title as string) || readableLabel,
        successMessage: a.successMessage as string | undefined,
      };
    }
    return {
      url: ((optionMap as any).url?.value as string) || '',
      method: (((optionMap as any).method?.value as string) || 'POST').toUpperCase(),
      confirmText: ((optionMap as any).confirm?.value as string) || '',
      colorRaw: (optionMap as any).color?.value as string | undefined,
      iconName: field.icon,
      title: value || readableLabel,
      successMessage: undefined as string | undefined,
    };
  }, [usingRef, refEntry, optionMap, field.icon, value, readableLabel]);

  const color: MuiColor = isMuiColor(resolved.colorRaw) ? resolved.colorRaw : 'primary';
  const StartIcon = resolveIcon(resolved.iconName);

  const [open, setOpen] = useState(false);
  const [busy, setBusy] = useState(false);

  const disabled = busy || !resolved.url || busyFromState || isPending(actionId);

  const fire = async () => {
    setBusy(true);
    try {
      if (!resolved.url) throw new Error(`action "${refId || field.label}" has no url`);
      const axiosPath = toAxiosPath(resolved.url);
      if (resolved.method === 'GET') await AXIOS.get(axiosPath);
      else if (resolved.method === 'DELETE') await AXIOS.delete(axiosPath);
      else if (resolved.method === 'PUT') await AXIOS.put(axiosPath, {});
      else await AXIOS.post(axiosPath, {});
      enqueueSnackbar(resolved.successMessage || `${resolved.title} — ok`, { variant: 'success' });

      // Optional follow-up: mark this action pending in form context so
      // sibling widgets (TableField loader, the button itself via
      // loadingIf) can show progress immediately, and fire a short
      // polling schedule to pick up the backend-side result (e.g. WiFi
      // scan results) without the user reopening the tab. Refetches run
      // silent so transient network hiccups during the radio's scan
      // window don't spawn error snackbars.
      if (refetchAfter) {
        markPending(actionId, PENDING_WINDOW_MS);
        REFETCH_SCHEDULE_MS.forEach((ms) => {
          window.setTimeout(() => { refetch({ silent: true }); }, ms);
        });
      }
    } catch (e: any) {
      enqueueSnackbar(extractErrorMessage(e, `${resolved.title} failed`), { variant: 'error' });
    } finally {
      setBusy(false);
      setOpen(false);
    }
  };

  const onClick = () => {
    if (resolved.confirmText) setOpen(true);
    else fire();
  };

  return (
    <ListItem>
      <Box sx={{ flexGrow: 1 }}>
        <Button
          variant="contained"
          color={color}
          onClick={onClick}
          disabled={disabled}
          startIcon={StartIcon ? <StartIcon /> : undefined}
        >
          {resolved.title}
        </Button>
      </Box>
      <Dialog open={open} onClose={() => setOpen(false)}>
        <DialogTitle>Confirm</DialogTitle>
        <DialogContent dividers>{resolved.confirmText}</DialogContent>
        <DialogActions>
          <Button variant="outlined" onClick={() => setOpen(false)} disabled={busy}>
            Cancel
          </Button>
          <Button variant="contained" color={color} onClick={fire} disabled={busy}>
            Confirm
          </Button>
        </DialogActions>
      </Dialog>
    </ListItem>
  );
};

export default ActionField;
