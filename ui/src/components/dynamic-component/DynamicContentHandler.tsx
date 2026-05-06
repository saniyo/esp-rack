import React, { FC, useEffect, useState } from 'react';
import {
  Box,
  Card,
  CardContent,
  CardHeader,
  Divider,
  IconButton,
  List,
  Tooltip,
  Typography,
} from '@mui/material';
import DeleteIcon from '@mui/icons-material/Delete';
import { Field } from './types';
import { parseFieldOptions, ShowIfSpec } from './utils/fieldParser';
import TextField from './elements/TextField';
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
import FilesField from './elements/FilesField';
import UploadField from './elements/UploadField';
import TableField from './elements/TableField';
import DateTimeField from './elements/DateTimeField';
import ActionField from './elements/ActionField';
import MessageField from './elements/MessageField';
import TimeZones from './elements/TimeZones';
import IPAddressField from './elements/IPAddressField';
import DefaultComponent from './elements/DefaultComponent';

interface DynamicContentHandlerProps {
  title: string;
  description: string;
  fields: Field[];
  onInputChange?: (label: string, value: any) => void;
  onFieldBlur?: (label: string, value: any) => void;
  onFieldFocus?: (label: string) => void;
  onSubmit?: (event: React.FormEvent) => void;
  hideTitle?: boolean;
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
}) => {
  const [formState, setFormState] = useState<Record<string, any>>({});

  useEffect(() => {
    const initialState: Record<string, any> = {};
    fields.forEach((field) => {
      initialState[field.label] = field.value;
    });
    setFormState(initialState);
  }, [fields]);

  const handleInputChange = (label: string, value: any) => {
    setFormState((prevState) => ({
      ...prevState,
      [label]: value,
    }));
    onInputChange && onInputChange(label, value);
  };

  const handleFieldBlur = (label: string, value: any) => {
    setFormState((prevState) => ({
      ...prevState,
      [label]: value,
    }));
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
        return <TextField key={fieldKey} {...commonProps} />;
      case 'secret':
        return <SecretField key={fieldKey} {...commonProps} />;
      case 'number':
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
        return <TrendChart key={fieldKey} {...commonProps} />;
      case 'files':
        return <FilesField key={fieldKey} field={field} />;
      case 'upload':
        return <UploadField key={fieldKey} field={field} />;
      case 'table':
        return <TableField key={fieldKey} field={field} value={formState[field.label]} onChange={handleInputChange} formState={formState} />;
      case 'datetime':
        return <DateTimeField key={fieldKey} {...commonProps} />;
      case 'action':
        return <ActionField key={fieldKey} field={field} value={formState[field.label]} />;
      case 'message':
        return <MessageField key={fieldKey} field={field} value={formState[field.label]} />;
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
  const renderAll = (): React.ReactNode[] => {
    const nodes: React.ReactNode[] = [];
    let bareAccumulator: React.ReactNode[] = [];
    let bareStart = 0;

    const flushBare = () => {
      if (bareAccumulator.length === 0) return;
      nodes.push(<List key={`list-${bareStart}`}>{bareAccumulator}</List>);
      bareAccumulator = [];
    };

    let i = 0;
    while (i < fields.length) {
      const f = fields[i];
      if (!isFieldVisible(f)) { i++; continue; }

      const parsedF = parseFieldOptions(f.label, f.o);
      const cardId = (parsedF.optionMap as any).card?.value as string | undefined;

      if (!cardId) {
        if (bareAccumulator.length === 0) bareStart = i;
        bareAccumulator.push(renderField(f, i));
        i++;
        continue;
      }

      // Flush any pending bare fields before emitting the Card so the Card
      // sits at the top level of the render tree, not inside a <ul>.
      flushBare();

      // Collect consecutive fields sharing the same card id (visibility-
      // filtered). The header title/deletable flag are read from the first
      // field in the span.
      const groupStart = i;
      const cardTitleText = (parsedF.optionMap as any).cardTitle?.value as string | undefined;
      const deletable = !!((parsedF.optionMap as any).cardDeletable?.value);

      const groupFields: { f: Field; absoluteIndex: number }[] = [];
      while (i < fields.length) {
        const gf = fields[i];
        const gpOpts = parseFieldOptions(gf.label, gf.o);
        const gCardId = (gpOpts.optionMap as any).card?.value as string | undefined;
        if (gCardId !== cardId) break;
        if (isFieldVisible(gf)) groupFields.push({ f: gf, absoluteIndex: i });
        i++;
      }

      const handleCardDelete = () => {
        groupFields.forEach(({ f: gf }) => {
          handleInputChange(gf.label, emptyValueFor(gf.type));
        });
      };

      nodes.push(
        <Card key={`card-${cardId}-${groupStart}`} variant="outlined" sx={{ mb: 2, overflow: 'visible' }}>
          {(cardTitleText || deletable) && (
            <CardHeader
              titleTypographyProps={{ variant: 'subtitle1' }}
              title={cardTitleText || ''}
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
            <List disablePadding>
              {groupFields.map(({ f: gf, absoluteIndex }) => renderField(gf, absoluteIndex))}
            </List>
          </CardContent>
        </Card>
      );
    }
    flushBare();
    return nodes;
  };

  return (
    <form onSubmit={onSubmit}>
      {!hideTitle && (
        <Typography variant="h4" gutterBottom>
          {title}
        </Typography>
      )}
      {description && (
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
