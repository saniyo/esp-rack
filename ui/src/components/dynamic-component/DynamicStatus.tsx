// DynamicStatus.tsx
import React, { FC, useEffect, useState, useCallback, useRef } from 'react';
import { Box } from '@mui/material';
import DynamicContentHandler from './DynamicContentHandler';
import LiveIndicator from './LiveIndicator';
import { SectionContent } from '../../components';
import FormLoader from '../loading/FormLoader';
import { Form, Field } from './types';
import { parseFieldOptions } from './utils/fieldParser';
import { setLiveValue } from './utils/liveCash';

interface DynamicStatusProps {
  formName: string;
  data: { description: string; fields: Array<{ [k: string]: any; o: string }> };
  wsData: any;
  connected: boolean;
  saving: boolean;
  originId: string;
  setData: (d: any) => void;
  disconnect: () => void;
  updateWsData: (d: React.SetStateAction<any>, tx?: boolean, clr?: boolean) => void;
}

const DynamicStatus: FC<DynamicStatusProps> = ({
  formName, data, wsData, connected,
  originId, updateWsData
}) => {

  const [formState, setFormState] = useState<Form>({
    title: formName, description: '', fields: []
  });

  const editingFields = useRef(new Set<string>());

  const getVal = (src: any, key: string) =>
    src?.[key] ?? src?.status?.fields?.[key];

  // Safety net: if the backend field forgot trendMaxPoints(N), fall back to
  // a sane default so a 10 Hz LightState stream can't balloon this array
  // into the tens-of-thousands-of-points range over a long tab session.
  const MERGE_TREND_DEFAULT_CAP = 1000;

  const mergeTrend = (oldArr: any[], fresh: any[], maxPts: number) => {
    const merged = [...oldArr];
    fresh.forEach((pt) => {
      const ts = pt.timestamp;
      const idx = merged.findIndex((it: any) => it.timestamp === ts);
      if (idx === -1) {
        merged.push({ ...pt });
      } else {
        merged[idx] = { ...merged[idx], ...pt };
      }
    });
    merged.sort((a: any, b: any) => a.timestamp - b.timestamp);
    const cap = maxPts > 0 ? maxPts : MERGE_TREND_DEFAULT_CAP;
    if (merged.length > cap)
      merged.splice(0, merged.length - cap);
    return merged;
  };

  // REST → initial (очікуємо boolean для двійкових)
  useEffect(() => {
    if (!data?.fields) return;

    const fields: (Field & { baseLabel: string })[] =
      data.fields.map((obj, idx) => {
        const o = obj.o ?? '';
        const base = Object.keys(obj)[0];
        let val = (obj as any)[base];

        const { optionMap, finalType } = parseFieldOptions(base, o);
        const label = finalType === 'trend'
          ? `${base}_${(o.match(/mode=([^;]+)/)?.[1] || idx)}`
          : base;

        if (finalType === 'checkbox' || finalType === 'switch' || finalType === 'button') {
          val = Boolean(val);
        }

        return {
          label,
          value: val,
          type: finalType,
          readOnly: !!optionMap.r,
          min: optionMap.mn ? parseFloat(optionMap.mn.value as string) : undefined,
          max: optionMap.mx ? parseFloat(optionMap.mx.value as string) : undefined,
          step: optionMap.st !== undefined ? Number(optionMap.st.value) : undefined,
          placeholder: optionMap.pl?.value as string | undefined,
          icon: optionMap.icon?.value as string | undefined,
          o,
          baseLabel: base
        } as Field & { baseLabel: string };
      });

    setFormState({ title: formName, description: data.description, fields });
  }, [data, formName]);

  // WS apply (очікуємо boolean для двійкових)
  const applyWs = useCallback((ws: any) => {
    setFormState((prev) => {
      const upd = prev.fields.map((f: any) => {
        const { label, type, o } = f;
        const base = f.baseLabel || label;

        if (type === 'trend') {
          const inc = getVal(ws, base);
          if (!Array.isArray(inc) || !inc.length) return f;

          const oldArr = Array.isArray(f.value) ? f.value : [];
          const maxPts = +(o.match(/maxPoints=(\d+)/)?.[1] || 0);

          return { ...f, value: mergeTrend(oldArr, inc, maxPts) };
        }

        if (type === 'table') {
          // Three merge modes, declared by the backend via the `mode=...`
          // option in the field's `o` string:
          //   replace  (default) — backend sends the full row set each push,
          //                        frontend overwrites verbatim
          //   append             — backend sends only the new rows; frontend
          //                        concats to existing and trims to maxRows
          //   upsert             — backend sends only rows that changed;
          //                        frontend matches by the first declared
          //                        column (cols=key|...) and either updates
          //                        the existing row in place or appends
          const inc = getVal(ws, base);
          if (!Array.isArray(inc)) return f;

          const tableMode = (o.match(/mode=([^;]+)/)?.[1] || 'replace').trim();
          const maxRows = +(o.match(/maxRows=(\d+)/)?.[1] || 0);

          if (tableMode === 'replace' || !tableMode) {
            return { ...f, value: inc };
          }

          const oldArr = Array.isArray(f.value) ? f.value : [];

          if (tableMode === 'append') {
            const merged = oldArr.concat(inc);
            const cap = maxRows > 0 ? maxRows : 1000;
            if (merged.length > cap) merged.splice(0, merged.length - cap);
            return { ...f, value: merged };
          }

          if (tableMode === 'upsert') {
            // First column key from `cols=<first>|...`. Fallback to 'id'.
            const colsRaw = o.match(/cols=([^;]+)/)?.[1] || '';
            const firstCol = colsRaw.split(',')[0]?.split('|')[0]?.trim() || 'id';

            const merged = [...oldArr];
            for (const row of inc) {
              const key = (row as any)[firstCol];
              const idx = merged.findIndex((r: any) => r && r[firstCol] === key);
              if (idx === -1) merged.push(row);
              else merged[idx] = { ...merged[idx], ...row };
            }
            const cap = maxRows > 0 ? maxRows : 1000;
            if (merged.length > cap) merged.splice(0, merged.length - cap);
            return { ...f, value: merged };
          }

          // Unknown mode — safest fallback is full replace so the UI keeps
          // working rather than freezing on an outdated row set.
          return { ...f, value: inc };
        }

        const v = getVal(ws, base);
        if (v === undefined) return f;

        if (type === 'checkbox' || type === 'switch' || type === 'button')
          return { ...f, value: Boolean(v) };

        return { ...f, value: v };
      });
      return { ...prev, fields: upd };
    });
  }, []);

  useEffect(() => { if (wsData) applyWs(wsData); }, [wsData, applyWs]);

  // input handlers — тільки boolean для двійкових
  const onChange = (lbl: string, val: any) => {
    editingFields.current.add(lbl);
    setFormState((p) => ({ ...p, fields: p.fields.map((f) => f.label === lbl ? { ...f, value: val } : f) }));
  };

  const onBlur = (lbl: string, val: any) => {
    const fld = formState.fields.find((f: any) => f.label === lbl);
    if (!fld) return;

    const isBinary = (fld.type === 'checkbox' || fld.type === 'switch' || fld.type === 'button');
    const uiVal = isBinary ? Boolean(val) : val;

    // по дроту відправляємо та ж boolean-семантика
    updateWsData({ type: 'p', origin_id: originId, p: { [lbl]: uiVal } }, true);

    // live кеш — boolean для двійкових
    setLiveValue(lbl, uiVal, 'ws');

    // Field is no longer under edit — release it from the tracking Set so
    // repeated edits across many fields don't grow this ref for the life of
    // the component.
    editingFields.current.delete(lbl);
  };

  if (!formState.fields.length)
    return <FormLoader message="Loading..." />;

  // Always render REST-sourced fields so the user sees data immediately;
  // the small Live/Offline indicator at the top signals whether WS pushes
  // are currently overlaying those values. Previously we blocked the whole
  // panel with "Waiting for WebSocket connection..." which hid the REST
  // snapshot even though it was already in hand.
  return (
    <SectionContent title={formState.title || 'Status'} titleGutter>
      <Box sx={{ display: 'flex', justifyContent: 'flex-end', mb: 1 }}>
        <LiveIndicator connected={connected} />
      </Box>
      <DynamicContentHandler
        title={formState.title}
        description={formState.description}
        fields={formState.fields}
        onInputChange={onChange}
        onFieldBlur={onBlur}
        hideTitle
      />
    </SectionContent>
  );
};

export default DynamicStatus;
