// DynamicSettings.tsx
import React, { FC, useState, useEffect, useRef } from 'react';
import { useLocation } from 'react-router-dom';
import {
  Button,
  Dialog,
  DialogActions,
  DialogContent,
  DialogContentText,
  DialogTitle,
} from '@mui/material';
import DynamicContentHandler from './DynamicContentHandler';
import { SectionContent } from '../../components';
import FormLoader from '../loading/FormLoader';
import SubmitButton from './elements/SubmitButton';
import { Field, Form } from './types';
import { parseFieldOptions } from './utils/fieldParser';
import {
  getLiveValue,
  getLiveSnapshot,
  subscribeLiveValues,
  setLiveValuesBulk,
} from './utils/liveCash';

interface DynamicSettingsProps {
  formName: string;
  data: { description: string; fields: Array<{ [key: string]: any; o: string }> };
  saveData: (updatedData: any) => void;
  saving: boolean;
  setData: (newData: any) => void;
  /** Hide the Save button — use for read-only (status) tabs. */
  readOnly?: boolean;
}

const DynamicSettings: FC<DynamicSettingsProps> = ({ formName, data, saveData, saving, readOnly = false }) => {
  const [formState, setFormState] = useState<Form>({
    title: formName,
    description: data.description || '',
    fields: [],
  });

  // Тут зберігаємо значення у форматі UI.
  // Для булевих — завжди true/false.
  const [changedFields, setChangedFields] = useState<Record<string, any>>({});

  const labelsRef = useRef<string[]>([]);

  // Cross-tab prefill: when another tab navigated here via rowNav, React
  // Router hands us an object of pre-filled values through location
  // state. The prefill must SURVIVE intervening REST refetches (which
  // periodically rebuild `data`) — otherwise the user clicks an SSID
  // on the Scan tab, switches to Settings, sees the SSID populate, and
  // then a heartbeat refetch wipes it back to whatever's on disk.
  //
  // Strategy: parse on the FIRST visit of each navigation key (tracked
  // by `prefillAppliedForKeyRef`) and stash into `pendingPrefillRef`.
  // Every subsequent fields-rebuild from `data` overlays the ref's
  // values. The ref is cleared by the Save action (prefill committed)
  // — and naturally overwritten by the next NEW navigation (different
  // key + different dynamicPrefill payload).
  const location = useLocation();
  const dynamicPrefill = (location.state as any)?.dynamicPrefill as Record<string, any> | undefined;
  // Optional second-slot candidate. When set, this DynamicSettings owns
  // the smart-slot decision (a different tab's TableField forwarded
  // BOTH options because IT couldn't see this tab's slot states). On
  // first visit of the navigation key we inspect the just-built fields
  // to decide which prefill to apply, or surface a dialog if both
  // slots are populated.
  const dynamicPrefillFallback = (location.state as any)?.dynamicPrefillFallback as Record<string, any> | undefined;
  const prefillAppliedForKeyRef = useRef<string | undefined>(undefined);
  const pendingPrefillRef = useRef<Record<string, any>>({});

  // Dialog state for the both-slots-filled case. When set, the rendered
  // dialog asks the operator which slot to overwrite; resolving applies
  // the chosen prefill and closes the dialog.
  const [slotDialog, setSlotDialog] = useState<{
    primary: Record<string, any>;
    fallback: Record<string, any>;
    primaryLabel: string;
    fallbackLabel: string;
  } | null>(null);

  // Підвантаження з REST: усе, що булеве — приводимо до boolean.
  useEffect(() => {
    if (!data || !Array.isArray(data.fields)) return;

    const fields: Field[] = data.fields.map((raw) => {
      const o = (raw as any).o || '';
      const fieldName = Object.keys(raw).find((k) => k !== 'o') || '';
      let fieldValue = (raw as any)[fieldName];

      // Якщо є live—значення, воно пріоритетне
      const live = getLiveValue(fieldName);
      if (live !== undefined) fieldValue = live;

      const { optionMap, finalType } = parseFieldOptions(fieldName, o);

      const isBoolType =
        finalType === 'checkbox' || finalType === 'switch' || finalType === 'button';
      if (isBoolType) fieldValue = !!fieldValue;

      const min = optionMap.mn ? parseFloat(optionMap.mn.value as string) : undefined;
      const max = optionMap.mx ? parseFloat(optionMap.mx.value as string) : undefined;
      // деякі конфіги можуть не мати "st" у типах — читаємо обережно
      const step =
        (optionMap as any).st !== undefined ? Number((optionMap as any).st.value) : undefined;
      const placeholder = optionMap.pl ? (optionMap.pl.value as string) : undefined;

      return {
        label: fieldName,
        value: fieldValue,
        type: finalType,
        readOnly: !!optionMap.r,
        min,
        max,
        step,
        placeholder,
        icon: optionMap.icon?.value as string | undefined,
        o,
      };
    });

    // First visit of this navigation key with a fresh prefill payload —
    // capture into the persistent ref. From here on every fields rebuild
    // overlays the ref onto server-fresh values.
    const isNewNav =
      !!dynamicPrefill &&
      Object.keys(dynamicPrefill).length > 0 &&
      prefillAppliedForKeyRef.current !== location.key;

    if (isNewNav) {
      // Smart-slot decision when the source tab forwarded BOTH a primary
      // and fallback prefill candidate (e.g. WiFi scan → settings).
      // Compare the FIRST key of each candidate against the matching
      // current value in our just-built fields:
      //   * primary slot empty → apply primary as today
      //   * primary used, fallback empty → apply fallback silently
      //   * both slots used → defer to the dialog; pendingPrefillRef is
      //     left empty so nothing pre-applies, then the dialog resolver
      //     populates the ref + state on operator choice.
      if (dynamicPrefillFallback && Object.keys(dynamicPrefillFallback).length > 0) {
        const primaryKey  = Object.keys(dynamicPrefill!)[0];
        const fallbackKey = Object.keys(dynamicPrefillFallback)[0];
        const valueOf = (k: string): string => {
          const f = fields.find((x) => x.label === k);
          if (!f) return '';
          return f.value === undefined || f.value === null ? '' : String(f.value);
        };
        const primaryUsed  = valueOf(primaryKey).length > 0;
        const fallbackUsed = valueOf(fallbackKey).length > 0;

        if (!primaryUsed) {
          pendingPrefillRef.current = { ...(dynamicPrefill as Record<string, any>) };
        } else if (!fallbackUsed) {
          pendingPrefillRef.current = { ...dynamicPrefillFallback };
        } else {
          // Both filled — surface the dialog. Defer the prefill apply
          // until the operator picks a slot.
          setSlotDialog({
            primary:        dynamicPrefill!,
            fallback:       dynamicPrefillFallback,
            primaryLabel:   valueOf(primaryKey),
            fallbackLabel:  valueOf(fallbackKey),
          });
        }
      } else {
        pendingPrefillRef.current = { ...(dynamicPrefill as Record<string, any>) };
      }
      prefillAppliedForKeyRef.current = location.key;
    }

    // Overlay any still-pending prefill onto fields. Survives REST
    // refetches mid-edit; cleared once the user presses Save.
    if (Object.keys(pendingPrefillRef.current).length > 0) {
      for (const f of fields) {
        if (Object.prototype.hasOwnProperty.call(pendingPrefillRef.current, f.label)) {
          f.value = pendingPrefillRef.current[f.label];
        }
      }
    }

    labelsRef.current = fields.map((f) => f.label);
    setFormState({ title: formName, description: data.description || '', fields });

    // changedFields bookkeeping:
    //   * fresh navigation with prefill → seed with prefill so Save sends them
    //   * no active prefill → behave as before, reset on every refetch
    //   * prefill still active (no new nav) → leave changedFields alone so
    //     the prefill values + any in-tab edits accumulated since aren't
    //     wiped by a heartbeat refetch
    if (isNewNav) {
      setChangedFields({ ...pendingPrefillRef.current });
    } else if (Object.keys(pendingPrefillRef.current).length === 0) {
      setChangedFields({});
    }
  }, [data, formName, dynamicPrefill, location.key]);

  // Одноразово накласти snapshot із кешу (в UI-форматі)
  useEffect(() => {
    if (!formState.fields.length) return;

    const snap = getLiveSnapshot(labelsRef.current);
    if (!Object.keys(snap).length) return;

    let changed = false;
    const nextFields = formState.fields.map((f) => {
      if (snap[f.label] !== undefined && snap[f.label] !== f.value) {
        changed = true;
        return { ...f, value: snap[f.label] };
      }
      return f;
    });

    if (changed) {
      setFormState((prev) => ({ ...prev, fields: nextFields }));
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [formState.fields.length]);

  // Підписка на live-оновлення (значення завжди в UI-форматі)
  useEffect(() => {
    const unsub = subscribeLiveValues((label: string, value: any) => {
      if (!labelsRef.current.includes(label)) return;

      setFormState((prev) => {
        const idx = prev.fields.findIndex((f) => f.label === label);
        if (idx === -1) return prev;

        const oldVal = prev.fields[idx].value;
        if (oldVal === value) return prev;

        const nextFields = [...prev.fields];
        nextFields[idx] = { ...nextFields[idx], value };
        return { ...prev, fields: nextFields };
      });
    });

    return unsub;
  }, []);

  const handleInputChange = (label: string, value: any) => {
    // Усе зберігаємо в UI-форматі: булеві — тільки true/false
    const field = formState.fields.find((f) => f.label === label);
    const isBoolType =
      field && (field.type === 'checkbox' || field.type === 'switch' || field.type === 'button');
    const uiValue = isBoolType ? !!value : value;

    setFormState((prevState) => {
      const updatedFields = prevState.fields.map((f) =>
        f.label === label ? { ...f, value: uiValue } : f
      );
      return { ...prevState, fields: updatedFields };
    });

    setChangedFields((prev) => ({ ...prev, [label]: uiValue }));
  };

  const handleFieldBlur = (label: string, value: any) => {
    // Те саме, що й handleInputChange: фіксуємо UI-значення
    const field = formState.fields.find((f) => f.label === label);
    const isBoolType =
      field && (field.type === 'checkbox' || field.type === 'switch' || field.type === 'button');
    const uiValue = isBoolType ? !!value : value;

    setFormState((prevState) => {
      const updatedFields = prevState.fields.map((f) =>
        f.label === label ? { ...f, value: uiValue } : f
      );
      return { ...prevState, fields: updatedFields };
    });

    setChangedFields((prev) => ({ ...prev, [label]: uiValue }));
  };

  const handleSubmit = (event: React.FormEvent) => {
    event.preventDefault();
    if (Object.keys(changedFields).length === 0) return;

    // 1) Оновлюємо live-кеш UI-значеннями (булеві — true/false)
    setLiveValuesBulk(changedFields, 'rest');

    // 2) Відправляємо як є — без жодних 0/1 перетворень
    saveData(changedFields);

    // 3) Prefill is committed — drop the overlay so subsequent data
    // refetches reflect the server's authoritative state, not the
    // stale prefill values.
    pendingPrefillRef.current = {};
    setChangedFields({});
  };

  // Slot-dialog resolver. Apply the chosen prefill onto current fields
  // immediately (so the operator sees the values populate), seed
  // pendingPrefillRef so subsequent REST refetches preserve them, and
  // mirror into changedFields so Save sends them.
  const applySlot = (which: 'primary' | 'fallback') => {
    if (!slotDialog) return;
    const chosen = which === 'primary' ? slotDialog.primary : slotDialog.fallback;
    pendingPrefillRef.current = { ...chosen };
    setFormState((prev) => ({
      ...prev,
      fields: prev.fields.map((f) =>
        Object.prototype.hasOwnProperty.call(chosen, f.label)
          ? { ...f, value: chosen[f.label] }
          : f
      ),
    }));
    setChangedFields((prev) => ({ ...prev, ...chosen }));
    setSlotDialog(null);
  };

  if (!formState.fields) {
    return <FormLoader message="Waiting for WebSocket connection..." />;
  }

  return (
    <SectionContent title={formState.title || 'Settings'} titleGutter>
      <DynamicContentHandler
        title={formState.title || 'Settings'}
        description={formState.description || ''}
        fields={formState.fields || []}
        onInputChange={handleInputChange}
        onFieldBlur={handleFieldBlur}
        onSubmit={handleSubmit}
        hideTitle
      />
      {!readOnly && <SubmitButton onSubmit={handleSubmit} saving={saving} />}
      {slotDialog && (
        <Dialog open onClose={() => setSlotDialog(null)} maxWidth="xs" fullWidth>
          <DialogTitle>Choose slot</DialogTitle>
          <DialogContent>
            <DialogContentText>
              Both slots are already configured. Which one should be replaced?
            </DialogContentText>
          </DialogContent>
          <DialogActions>
            <Button onClick={() => setSlotDialog(null)} color="inherit">Cancel</Button>
            <Button onClick={() => applySlot('primary')} color="primary">
              Replace primary{slotDialog.primaryLabel ? ` (${slotDialog.primaryLabel})` : ''}
            </Button>
            <Button onClick={() => applySlot('fallback')} color="primary">
              Replace backup{slotDialog.fallbackLabel ? ` (${slotDialog.fallbackLabel})` : ''}
            </Button>
          </DialogActions>
        </Dialog>
      )}
    </SectionContent>
  );
};

export default DynamicSettings;
