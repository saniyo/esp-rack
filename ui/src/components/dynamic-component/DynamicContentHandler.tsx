import { ownerOfAction } from '../../contexts/manifest/normalizeManifest';
import {
  CELL_SX, fullOf, gridColumnFor, gridTemplateFor, groupByCard, isLayoutMarker, segmentFields, spanOf,
  tileOf, visibleHeading,
} from './utils/formLayout';
import { HeadingContext } from './HeadingContext';
import React, { FC, useEffect, useRef, useState, useContext } from 'react';
import {
  Box,
  Card,
  CardContent,
  CardHeader,
  IconButton,
  Tooltip,
  Typography,
} from '@mui/material';
import DeleteIcon from '@mui/icons-material/Delete';
import { Field } from './types';
import { parseFieldOptions, ShowIfSpec } from './utils/fieldParser';
import TextField from './elements/TextField';
import TileField from './elements/TileField';
import SecretField from './elements/SecretField';
import NumberField from './elements/NumberField';
import SliderField from './elements/SliderField';
import Checkbox from './elements/Checkbox';
import ButtonField from './elements/ButtonField';
import Switch from './elements/Switch';
import Dropdown from './elements/Dropdown';
import Textarea from './elements/TextArea';
import RadioField from './elements/RadioField';
import TrendChart from './elements/TrendChart';
import ProgressField from './elements/ProgressField';
import FlowField from './elements/FlowField';
import FilesField from './elements/FilesField';
import UploadField from './elements/UploadField';
import TableField from './elements/TableField';
import DateTimeField from './elements/DateTimeField';
import ActionField from './elements/ActionField';
import MessageField from './elements/MessageField';
import TimeZones from './elements/TimeZones';
import IPAddressField from './elements/IPAddressField';
import DefaultComponent from './elements/DefaultComponent';
import EndpointMissing from './elements/EndpointMissing';
import { useDynamicForm } from './DynamicFormContext';
import { useManifest } from '../../contexts/manifest';
import { endpointAvailable, findAction } from '../../utils/manifestEndpoints';

interface DynamicContentHandlerProps {
  title: string;
  description: string;
  fields: Field[];
  onInputChange?: (label: string, value: any) => void;
  onFieldBlur?: (label: string, value: any) => void;
  onFieldFocus?: (label: string) => void;
  onSubmit?: (event: React.FormEvent) => void;
  hideTitle?: boolean;
  // Latest live state of the tab's channel (every readState key, shallow).
  // Widgets that reference a sibling VALUE (progress maxKey/markerKey,
  // trend refl/vrefl) look it up here first, then among the form's fields.
  liveValues?: Record<string, any>;
}

// Returns what "empty" looks like for a given field type, used when a card's
// Delete button clears every field in its group.
const emptyValueFor = (type?: string): any => {
  switch (type) {
    case 'checkbox':
    case 'switch':
    case 'button':
      return false;
    case 'number':
    case 'slider':
      return 0;
    case 'table':
      return [];
    default:
      return '';
  }
};

const DynamicContentHandler: FC<DynamicContentHandlerProps> = ({
  title,
  description,
  fields,
  onInputChange,
  onFieldBlur,
  onSubmit,
  hideTitle = false,
  liveValues,
}) => {
  const [formState, setFormState] = useState<Record<string, any>>({});
  const seenHeadings = useContext(HeadingContext);
  // Track the field.value we last applied per label. On every fields-prop
  // update we adopt the server's new value ONLY for labels whose
  // field.value actually changed since the previous render — that way
  // user edits in formState (typing into a TextField, or rowFill from a
  // sibling TableField) survive WS-driven re-renders of UNRELATED fields.
  // Without this guard, every WS push (status text, live table tick,
  // anything mutating field.value somewhere on the form) reset the
  // ENTIRE formState back to field.value, wiping pending row-click
  // selections / mid-typing input on every tick of a busy stream.
  const prevFieldValuesRef = useRef<Record<string, any>>({});

  useEffect(() => {
    setFormState((prev) => {
      const next: Record<string, any> = { ...prev };
      let changed = false;
      fields.forEach((field) => {
        const recorded = prevFieldValuesRef.current[field.label];
        const incoming = field.value;
        // First-render seed (no recorded prior value AND no formState
        // entry yet) — adopt server's value verbatim.
        if (recorded === undefined && !(field.label in prev)) {
          next[field.label] = incoming;
          changed = true;
        } else if (incoming !== recorded) {
          // Server pushed a new value for this field since last render
          // — adopt it. (This is intentionally aggressive: for fields
          // the operator was editing AT THE EXACT MOMENT the server
          // pushed, the server still wins. TextField's internal
          // isEditing flag handles the typing case for itself; this
          // path only matters for non-text widgets that read formState
          // directly without buffering.)
          next[field.label] = incoming;
          changed = true;
        }
        // else: server didn't change this field's value → keep prev
        // (which may carry a user edit not yet pushed to backend).
      });

      // Refresh the ref AFTER computing next so subsequent renders
      // compare against the values we just observed.
      fields.forEach((field) => {
        prevFieldValuesRef.current[field.label] = field.value;
      });

      return changed ? next : prev;
    });
  }, [fields]);

  // Mirror handleInputChange writes into the form-level live ref so
  // sibling components (notably ActionField under the same
  // DynamicFormContext) can read the operator's CURRENT input —
  // including transient row-click selections — at fire-time. Without
  // this the context's `formState` only reflects the last server
  // GET, and `withFields` ships an empty string the moment an action
  // button is clicked after a row pick.
  const { setLiveValue } = useDynamicForm();

  // Manifest snapshot used by the endpoint-availability check below.
  // ManifestLoader gates children on `revealComplete`, so by the time
  // this component mounts the manifest is final and findFeature returns
  // a stable result across renders. Used to detect widgets whose backend
  // (FileSystemModule, an OTA endpoint, an action whose owning module
  // was opted out, …) wasn't installed on this build — those widgets
  // would otherwise spinner-forever or 404-snackbar-spam.
  const { manifest, ensureActions, actionsLoaded } = useManifest();

  const handleInputChange = (label: string, value: any) => {
    setFormState((prevState) => ({
      ...prevState,
      [label]: value,
    }));
    setLiveValue?.(label, value);
    onInputChange && onInputChange(label, value);
  };

  const handleFieldBlur = (label: string, value: any) => {
    setFormState((prevState) => ({
      ...prevState,
      [label]: value,
    }));
    setLiveValue?.(label, value);
    onFieldBlur && onFieldBlur(label, value);
  };

  // Conditional visibility: if the field carries `showIf=<dep>:<val>` in its
  // options, render it only when the current form state has `formState[dep]`
  // stringifying equal to `val`. This powers e.g. Static IP fields that
  // only appear when the static_ip_config switch is on.
  const isFieldVisible = (field: Field): boolean => {
    const { optionMap } = parseFieldOptions(field.label, field.o);
    const spec = optionMap.showIf?.value as ShowIfSpec | undefined;
    if (!spec || !spec.field) return true;
    const depVal = formState[spec.field];
    if (depVal === undefined || depVal === null) return false;
    return String(depVal) === spec.val;
  };

  const getSibling = (key: string): any => {
    const live = liveValues?.[key];
    return live !== undefined ? live : formState[key];
  };

  const renderField = (field: Field, index: number): JSX.Element => {
    const fieldKey = `${field.label}-${index}`;
    const commonProps = {
      field,
      value: formState[field.label],
      onChange: handleInputChange,
      onBlur: (lbl: string, val: any) => handleFieldBlur(lbl, val),
    };

    switch (field.type) {
      case 'text':
        if (tileOf(field.o)) return <TileField key={fieldKey} field={field} value={formState[field.label]} getSibling={getSibling} />;
        return <TextField key={fieldKey} {...commonProps} />;
      case 'secret':
        return <SecretField key={fieldKey} {...commonProps} />;
      case 'number':
        if (tileOf(field.o)) return <TileField key={fieldKey} field={field} value={formState[field.label]} getSibling={getSibling} />;
        return <NumberField key={fieldKey} {...commonProps} />;
      case 'slider':
        return <SliderField key={fieldKey} {...commonProps} />;
      case 'checkbox':
        return <Checkbox key={fieldKey} {...commonProps} />;
      case 'button':
        return <ButtonField key={fieldKey} {...commonProps} />;
      case 'switch':
        return <Switch key={fieldKey} {...commonProps} />;
      case 'dropdown':
        return <Dropdown key={fieldKey} {...commonProps} />;
      case 'textarea':
        return <Textarea key={fieldKey} {...commonProps} />;
      case 'radio':
        return <RadioField key={fieldKey} {...commonProps} />;
      case 'trend':
        return <TrendChart key={fieldKey} {...commonProps} getSibling={getSibling} />;
      case 'progress':
        return (
          <ProgressField key={fieldKey} field={field} value={formState[field.label]} getSibling={getSibling} />
        );
      case 'flow':
        return <FlowField key={fieldKey} field={field} getSibling={getSibling} />;
      case 'files': {
        const { optionMap: om } = parseFieldOptions(field.label, field.o);
        const base = ((om as any).base?.value as string) || '/api/v1/fs';
        if (!endpointAvailable(manifest, base)) {
          return (
            <EndpointMissing
              key={fieldKey}
              label={field.label}
              endpoint={`${base}/*`}
              reason="FileSystem module not installed — add .install<FileSystemModule>() to the composition root."
            />
          );
        }
        return <FilesField key={fieldKey} field={field} />;
      }
      case 'upload': {
        const { optionMap: om } = parseFieldOptions(field.label, field.o);
        const url = (om as any).url?.value as string | undefined;
        if (url && !endpointAvailable(manifest, url)) {
          return (
            <EndpointMissing
              key={fieldKey}
              label={field.label}
              endpoint={url}
              reason="Upload endpoint not registered — the backend module that exposes this URL isn't installed."
            />
          );
        }
        return <UploadField key={fieldKey} field={field} />;
      }
      case 'table':
        return <TableField key={fieldKey} field={field} value={formState[field.label]} onChange={handleInputChange} formState={formState} />;
      case 'datetime':
        return <DateTimeField key={fieldKey} {...commonProps} />;
      case 'action': {
        const { optionMap: om } = parseFieldOptions(field.label, field.o);
        const ref = (om as any).ref?.value as string | undefined;
        const url = (om as any).url?.value as string | undefined;
        if (ref && !findAction(manifest, ref)) {
          // A feature's own action may still be on its way, or the loaded
          // list predates a firmware update: fetched once more first.
          const owner = ownerOfAction(ref);
          if (owner && manifest.features.some((x) => x.id === owner) && !actionsLoaded(owner, ref)) {
            ensureActions(owner, ref);
            return <React.Fragment key={fieldKey} />;
          }
          return (
            <EndpointMissing
              key={fieldKey}
              label={field.label}
              endpoint={`actionRef:${ref}`}
              reason="Action not registered — the owning backend module isn't installed."
            />
          );
        }
        if (!ref && url && !endpointAvailable(manifest, url)) {
          return (
            <EndpointMissing
              key={fieldKey}
              label={field.label}
              endpoint={url}
              reason="Action endpoint not registered — the backend module that handles this URL isn't installed."
            />
          );
        }
        return <ActionField key={fieldKey} field={field} value={formState[field.label]} />;
      }
      case 'message':
        return <MessageField key={fieldKey} field={field} value={formState[field.label]} getSibling={getSibling} />;
      case 'timezones':
        return <TimeZones key={fieldKey} {...commonProps} />;
      case 'ip':
        return <IPAddressField key={fieldKey} {...commonProps} />;
      default:
        return <DefaultComponent key={fieldKey} field={field} />;
    }
  };

  // Build the render stream in order. Fields with the same `card=<id>` tag
  // appearing consecutively are collapsed into a single MUI Card so the
  // backend can compose "slot"-style UIs (e.g. primary/secondary WiFi pairs)
  // out of the plain field primitives without bespoke components.
  //
  // Between card groups, consecutive non-card fields get wrapped in a single
  // <List> — each field renders as a <ListItem> internally, so without a
  // parent <ul> inset Dividers (component="li") would be stray.
  // fields between layout markers form one CSS grid. A grid right after
  // another one keeps the rows' gap (Cols(4) then Cols(5) don't touch).
  const renderGrid = (entries: { f: Field; i: number }[], keyBase: string): React.ReactNode[] =>
    segmentFields(entries, (e) => e.f).map((seg, s) => (
      <Box
        key={`${keyBase}-${s}`}
        sx={{
          display: 'grid',
          gap: 2,
          mt: s ? 2 : 0,
          alignItems: 'stretch',
          gridTemplateColumns: gridTemplateFor(seg.cols),
        }}
      >
        {seg.cells.map(({ f: cf, i: ci }) => (
          // A list of its own: the fields render <li> items (and <li> dividers),
          // which show bullets outside a list.
          <Box
            component="ul"
            key={`cell-${ci}`}
            sx={{
              listStyle: 'none',
              m: 0,
              p: 0,
              minWidth: 0,
              gridColumn: gridColumnFor(cf.type, spanOf(cf.o), fullOf(cf.o), seg.cols, cf.readOnly || tileOf(cf.o)),
              ...CELL_SX,
            }}
          >
            {renderField(cf, ci)}
          </Box>
        ))}
      </Box>
    ));

  const cardOf = (f: Field): string | undefined =>
    (parseFieldOptions(f.label, f.o).optionMap as any).card?.value as string | undefined;

  const renderAll = (): React.ReactNode[] => {
    const nodes: React.ReactNode[] = [];
    // Hidden fields are left out first: they neither show nor split a card.
    // Each card (and each bare run between cards) owns the layout markers
    // in front of it (groupByCard).
    const visible = fields.map((f, i) => ({ f, i })).filter(({ f }) => isLayoutMarker(f) || isFieldVisible(f));
    const groups = groupByCard(visible, (e) => e.f, (e) => (isLayoutMarker(e.f) ? undefined : cardOf(e.f)));

    for (const group of groups) {
      const groupStart = group.entries[0].i;
      const cardId = group.card;
      if (!cardId) {
        // Bare fields: one grid per segment, at the top level (not in a Card).
        nodes.push(...renderGrid(group.entries, `bare-${groupStart}`));
        continue;
      }

      // The header title/deletable flag are read from the card's first field.
      const first = group.entries.find(({ f }) => !isLayoutMarker(f))!.f;
      const firstOpts = parseFieldOptions(first.label, first.o).optionMap as any;
      const cardTitleText = firstOpts.cardTitle?.value as string | undefined;
      const deletable = !!firstOpts.cardDeletable?.value;

      const handleCardDelete = () => {
        group.entries.forEach(({ f: gf }) => {
          if (!isLayoutMarker(gf)) handleInputChange(gf.label, emptyValueFor(gf.type));
        });
      };

      nodes.push(
        <Card key={`card-${cardId}-${groupStart}`} variant="outlined" sx={{ mb: 2, overflow: 'visible' }}>
          {(visibleHeading(cardTitleText, [...seenHeadings, title]) || deletable) && (
            <CardHeader
              titleTypographyProps={{ variant: 'subtitle1' }}
              title={visibleHeading(cardTitleText, [...seenHeadings, title])}
              action={
                deletable ? (
                  <Tooltip title="Clear">
                    <IconButton size="small" onClick={handleCardDelete} aria-label="clear card">
                      <DeleteIcon fontSize="small" />
                    </IconButton>
                  </Tooltip>
                ) : null
              }
            />
          )}
          <CardContent sx={{ pt: 0 }}>
            {renderGrid(group.entries, `card-${cardId}`)}
          </CardContent>
        </Card>
      );
    }
    return nodes;
  };

  return (
    <form onSubmit={onSubmit}>
      {!hideTitle && visibleHeading(title, seenHeadings) && (
        <Typography variant="h4" gutterBottom>
          {title}
        </Typography>
      )}
      {visibleHeading(description, [...seenHeadings, title]) && (
        <Typography variant="body1" gutterBottom>
          {description}
        </Typography>
      )}
      <Box>
        {renderAll()}
      </Box>
    </form>
  );
};

export default DynamicContentHandler;
