import React, { FC, useMemo, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import {
  ListItem,
  ListItemAvatar,
  ListItemText,
  ListItemIcon,
  ListItemButton,
  List,
  Avatar,
  Badge,
  Button,
  CircularProgress,
  Dialog,
  DialogActions,
  DialogContent,
  DialogContentText,
  DialogTitle,
  Divider,
  Paper,
  Table,
  TableBody,
  TableCell,
  TableContainer,
  TableHead,
  TableRow,
  Typography,
  Box,
} from '@mui/material';
import TableChartIcon from '@mui/icons-material/TableChart';

import { parseFieldOptions, RowFillPair, BadgeSpec, ShowIfSpec } from '../utils/fieldParser';
import { FieldAvatar, resolveIcon } from '../utils/iconRegistry';

interface ColumnSpec {
  key: string;
  label: string;
  type: 'text' | 'number' | 'boolean';
  fmt?: string;
}

interface TableFieldProps {
  field: {
    label: string;
    o: string;
    value?: Array<Record<string, any>> | Record<string, any>;
  };
  value?: Array<Record<string, any>> | Record<string, any>;
  // When set, clicking a row dispatches onChange for every `rowFill` pair
  // parsed from field.o. Passed in by DynamicContentHandler; omitted when
  // the table is used in a read-only context.
  onChange?: (label: string, value: any) => void;
  // Current form-scoped state map (label→value). Consulted by the
  // `loadingIf` tag so the empty state can swap in a CircularProgress
  // while a sibling status field (e.g. "scan_status") signals "Scanning…".
  formState?: Record<string, any>;
}

// cols=col1|label=X|type=Y|fmt=Z,col2|label=A|type=B
function parseCols(raw: string): ColumnSpec[] {
  if (!raw) return [];
  return raw
    .split(',')
    .map((chunk) => chunk.trim())
    .filter(Boolean)
    .map((chunk) => {
      const parts = chunk.split('|').map((p) => p.trim());
      const key = parts[0];
      const kv: Record<string, string> = {};
      for (let i = 1; i < parts.length; i++) {
        const eq = parts[i].indexOf('=');
        if (eq > 0) {
          kv[parts[i].slice(0, eq)] = parts[i].slice(eq + 1);
        }
      }
      const t = (kv.type || 'text').toLowerCase();
      const type: ColumnSpec['type'] =
        t === 'number' || t === 'boolean' ? (t as any) : 'text';
      return {
        key,
        label: kv.label || key,
        type,
        fmt: kv.fmt,
      };
    });
}

function formatCell(val: any, col: ColumnSpec): string {
  if (val === undefined || val === null) return '';
  if (col.type === 'boolean') return val ? '✓' : '✗';
  if (col.type === 'number') {
    const n = typeof val === 'number' ? val : parseFloat(val);
    if (!Number.isFinite(n)) return String(val);
    if (col.fmt) {
      const decMatch = col.fmt.match(/\.(\d+)/);
      if (decMatch) return n.toFixed(decMatch[1].length);
      if (/^0+$/.test(col.fmt)) return n.toFixed(0);
    }
    return String(n);
  }
  return String(val);
}

// Replace {key} placeholders in a template with row values. Unknown keys
// render empty so partial rows don't break the layout.
const interpolate = (tmpl: string, row: Record<string, any>): string =>
  (tmpl || '').replace(/\{(\w+)\}/g, (_, k) => {
    const v = row[k];
    return v === undefined || v === null ? '' : String(v);
  });

const TableField: FC<TableFieldProps> = ({ field, value, onChange, formState }) => {
  const navigate = useNavigate();
  const parsed = useMemo(() => parseFieldOptions(field.label, field.o), [field.label, field.o]);
  const cols = useMemo(() => {
    const raw = (parsed.optionMap.cols?.value as string) || '';
    return parseCols(raw);
  }, [parsed.optionMap.cols]);

  const rowFill: RowFillPair[] = useMemo(() => {
    const v = (parsed.optionMap as any).rowFill?.value;
    return Array.isArray(v) ? (v as RowFillPair[]) : [];
  }, [parsed.optionMap]);

  const rowFillFallback: RowFillPair[] = useMemo(() => {
    const v = (parsed.optionMap as any).rowFillFallback?.value;
    return Array.isArray(v) ? (v as RowFillPair[]) : [];
  }, [parsed.optionMap]);

  const rowNavPath: string = useMemo(() => {
    const v = (parsed.optionMap as any).rowNav?.value;
    return typeof v === 'string' ? v : '';
  }, [parsed.optionMap]);

  // Smart-slot disambiguation. When the user clicks a row but BOTH the
  // primary and fallback slots are already populated, we surface a
  // dialog asking which one to overwrite. Pending row + chosen pair set
  // are committed once the operator picks.
  const [slotDialog, setSlotDialog] = useState<{
    row: Record<string, any>;
    primaryLabel: string;
    fallbackLabel: string;
  } | null>(null);

  const layoutMode = ((parsed.optionMap as any).layout?.value as string | undefined) || 'grid';
  const primaryKey = (parsed.optionMap as any).primary?.value as string | undefined;
  const secondaryTmpl = (parsed.optionMap as any).secondary?.value as string | undefined;
  const avatarKey = (parsed.optionMap as any).avatar?.value as string | undefined;
  const badgeSpec = (parsed.optionMap as any).badge?.value as BadgeSpec | undefined;

  const clickable = (!!onChange && rowFill.length > 0) || rowNavPath.length > 0;

  // Build a {target → value} prefill map from a rowFill pair set + a
  // clicked row. Empty `col` means "clear the target".
  const buildPrefill = (
    pairs: RowFillPair[],
    row: Record<string, any>,
  ): Record<string, any> => {
    const out: Record<string, any> = {};
    pairs.forEach(({ target, col }) => {
      if (!target) return;
      out[target] = col ? (row[col] ?? '') : '';
    });
    return out;
  };

  const isFilled = (target: string | undefined): boolean => {
    if (!target || !formState) return false;
    const v = formState[target];
    return v !== undefined && v !== null && String(v).length > 0;
  };

  const handleRowClick = (row: Record<string, any>) => {
    if (!clickable) return;

    const primary  = buildPrefill(rowFill, row);
    const fallback = rowFillFallback.length > 0 ? buildPrefill(rowFillFallback, row) : undefined;

    // CROSS-TAB navigation (rowNav set). The source tab has zero
    // visibility into the destination tab's form state — the SSID /
    // password fields live on a different DynamicSettings instance.
    // Defer the smart-slot decision to the destination by passing
    // BOTH prefill candidates through router state.
    if (rowNavPath) {
      navigate(rowNavPath, {
        state: {
          dynamicPrefill: primary,
          ...(fallback ? { dynamicPrefillFallback: fallback } : {}),
        },
      });
      return;
    }

    // SAME-TAB dispatch — formState IS the relevant state. Source-side
    // smart-slot logic works here because the SSID-shape field is in
    // the same form. (Currently no service uses this path; kept for
    // symmetry should a future form embed scan + settings together.)
    if (!fallback || !formState) {
      Object.entries(primary).forEach(([t, v]) => onChange?.(t, v));
      return;
    }

    const primaryKey  = rowFill[0]?.target;
    const fallbackKey = rowFillFallback[0]?.target;
    const primaryUsed  = isFilled(primaryKey);
    const fallbackUsed = isFilled(fallbackKey);

    if (!primaryUsed) {
      Object.entries(primary).forEach(([t, v]) => onChange?.(t, v));
      return;
    }
    if (!fallbackUsed) {
      Object.entries(fallback).forEach(([t, v]) => onChange?.(t, v));
      return;
    }
    setSlotDialog({
      row,
      primaryLabel:  String(formState[primaryKey!]  ?? ''),
      fallbackLabel: String(formState[fallbackKey!] ?? ''),
    });
  };

  // Same-tab dialog resolver. Cross-tab path skips the source dialog
  // entirely — the destination DynamicSettings shows its own dialog
  // because only it can compare the candidate prefills against the
  // currently-filled SSID slots.
  const resolveSlotDialog = (which: 'primary' | 'fallback') => {
    if (!slotDialog) return;
    const pairs = which === 'primary' ? rowFill : rowFillFallback;
    const prefill = buildPrefill(pairs, slotDialog.row);
    Object.entries(prefill).forEach(([t, v]) => onChange?.(t, v));
    setSlotDialog(null);
  };

  const slotDialogJsx = slotDialog ? (
    <Dialog open onClose={() => setSlotDialog(null)} maxWidth="xs" fullWidth>
      <DialogTitle>Choose slot</DialogTitle>
      <DialogContent>
        <DialogContentText>
          Both slots are already configured. Which one should be replaced?
        </DialogContentText>
      </DialogContent>
      <DialogActions>
        <Button onClick={() => setSlotDialog(null)} color="inherit">Cancel</Button>
        <Button onClick={() => resolveSlotDialog('primary')} color="primary">
          Replace primary{slotDialog.primaryLabel ? ` (${slotDialog.primaryLabel})` : ''}
        </Button>
        <Button onClick={() => resolveSlotDialog('fallback')} color="primary">
          Replace backup{slotDialog.fallbackLabel ? ` (${slotDialog.fallbackLabel})` : ''}
        </Button>
      </DialogActions>
    </Dialog>
  ) : null;

  const rows: Array<Record<string, any>> = useMemo(() => {
    const src = value ?? field.value;
    return Array.isArray(src) ? src : [];
  }, [value, field.value]);

  const fallbackIcon = (parsed.optionMap as any).icon?.value as string | undefined;
  const hideAvatar = !!((parsed.optionMap as any).hideAvatar?.value);

  // Loading indicator — `loadingIf="<field>:<prefix>"` matches via
  // startsWith against form state so localised strings like "Scanning…"
  // still fire on backend emitting "Scanning".
  const loadingSpec = (parsed.optionMap as any).loadingIf?.value as ShowIfSpec | undefined;
  const isLoading = !!(loadingSpec && loadingSpec.field && formState &&
    typeof formState[loadingSpec.field] !== 'undefined' &&
    String(formState[loadingSpec.field]).startsWith(loadingSpec.val));

  // -------- Cards layout (WiFi-scan style) --------
  if (layoutMode === 'cards') {
    const renderCard = (row: Record<string, any>, idx: number) => {
      const avatarIconName = avatarKey ? (row[avatarKey] as string | undefined) : fallbackIcon;
      const AvatarIcon = resolveIcon(avatarIconName);

      const primaryText = primaryKey ? String(row[primaryKey] ?? '') : '';
      const secondaryText = secondaryTmpl ? interpolate(secondaryTmpl, row) : '';

      const badgeValue = badgeSpec && badgeSpec.field
        ? `${row[badgeSpec.field] ?? ''}${badgeSpec.unit ? ` ${badgeSpec.unit.trim()}` : ''}`
        : undefined;
      const BadgeIcon = badgeSpec && badgeSpec.iconName ? resolveIcon(badgeSpec.iconName) : null;

      const inner = (
        <>
          <ListItemAvatar>
            <Avatar>{AvatarIcon ? <AvatarIcon /> : null}</Avatar>
          </ListItemAvatar>
          <ListItemText primary={primaryText} secondary={secondaryText || undefined} />
          {badgeSpec && BadgeIcon && (
            <ListItemIcon sx={{ minWidth: 0, pl: 1 }}>
              <Badge
                badgeContent={badgeValue}
                color="default"
                sx={{ '& .MuiBadge-badge': { position: 'relative', transform: 'none' } }}
              >
                <BadgeIcon />
              </Badge>
            </ListItemIcon>
          )}
        </>
      );

      return clickable ? (
        <ListItemButton key={idx} onClick={() => handleRowClick(row)}>
          {inner}
        </ListItemButton>
      ) : (
        <ListItem key={idx}>{inner}</ListItem>
      );
    };

    if (rows.length === 0) {
      if (isLoading) {
        return (
          <Box sx={{ display: 'flex', justifyContent: 'center', alignItems: 'center', py: 3 }}>
            <CircularProgress size={36} />
          </Box>
        );
      }
      // With hideAvatar the field contributes nothing when empty — callers
      // use this for pure-data feeds (e.g. WiFi scan results) where the
      // surrounding chrome would be noise until data arrives.
      if (hideAvatar) return null;
      return (
        <>
          <ListItem>
            <ListItemAvatar>
              <FieldAvatar iconName={fallbackIcon} fallback={TableChartIcon} />
            </ListItemAvatar>
            <ListItemText
              primary={<Typography variant="subtitle1">{parsed.readableLabel}</Typography>}
              secondary={
                <Typography variant="caption" color="text.secondary">
                  no items
                </Typography>
              }
            />
          </ListItem>
          <Divider />
        </>
      );
    }

    return (
      <>
        <List dense disablePadding>
          {rows.map((r, i) => (
            <React.Fragment key={i}>
              {renderCard(r, i)}
              {i < rows.length - 1 && <Divider variant="inset" component="li" />}
            </React.Fragment>
          ))}
        </List>
        {slotDialogJsx}
      </>
    );
  }

  // -------- Default: grid layout --------
  return (
    <>
      {!hideAvatar && (
        <ListItem>
          <ListItemAvatar>
            <FieldAvatar
              iconName={(parsed.optionMap as any).icon?.value as string | undefined}
              colorName={(parsed.optionMap as any).color?.value as string | undefined}
              fallback={TableChartIcon}
            />
          </ListItemAvatar>
          <ListItemText
            primary={<Typography variant="subtitle1">{parsed.readableLabel}</Typography>}
            secondary={
              <Typography variant="caption" color="text.secondary">
                {rows.length} row{rows.length === 1 ? '' : 's'}
              </Typography>
            }
          />
        </ListItem>
      )}
      <Box sx={{ px: 2, pb: 2 }}>
        <TableContainer component={Paper} variant="outlined">
          <Table size="small">
            <TableHead>
              <TableRow>
                {cols.map((c) => (
                  <TableCell key={c.key} align={c.type === 'number' ? 'right' : 'left'}>
                    {c.label}
                  </TableCell>
                ))}
              </TableRow>
            </TableHead>
            <TableBody>
              {rows.length === 0 ? (
                <TableRow>
                  <TableCell colSpan={Math.max(cols.length, 1)} align="center">
                    <Typography variant="caption" color="text.secondary">
                      no rows
                    </Typography>
                  </TableCell>
                </TableRow>
              ) : (
                rows.map((r, i) => (
                  <TableRow
                    key={i}
                    hover
                    onClick={clickable ? () => handleRowClick(r) : undefined}
                    sx={clickable ? { cursor: 'pointer' } : undefined}
                  >
                    {cols.map((c) => (
                      <TableCell key={c.key} align={c.type === 'number' ? 'right' : 'left'}>
                        {formatCell(r[c.key], c)}
                      </TableCell>
                    ))}
                  </TableRow>
                ))
              )}
            </TableBody>
          </Table>
        </TableContainer>
      </Box>
      <Divider />
      {slotDialogJsx}
    </>
  );
};

export default TableField;
