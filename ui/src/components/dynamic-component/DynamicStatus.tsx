// DynamicStatus.tsx
import { HeadingContext } from './HeadingContext';
import { visibleHeading } from './utils/formLayout';
import { markerFromRaw } from './utils/formLayout';
import React, { FC, useEffect, useMemo, useState, useCallback, useRef, useContext } from 'react';
import DynamicContentHandler from './DynamicContentHandler';
import TabActionsBar from './TabActionsBar';
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
  // Per-message heartbeat counter from useWs — bumped on every inbound
  // frame. Forwarded to LiveIndicator so its dot pulses once per
  // received message instead of pulsing constantly while connected.
  messageTick?: number;
  // A postable tab writes a committed field through REST (the device answers;
  // see DynamicFeature). Without it the field goes over the socket.
  saveField?: (patch: Record<string, any>) => void | Promise<void>;
  // Tells the channel which arrays are full sets (category trends,
  // replace-mode tables), so coalesced frames don't concatenate them.
  registerReplaceKeys?: (keys: string[]) => () => void;
}

const DynamicStatus: FC<DynamicStatusProps> = ({
  formName, data, wsData, connected,
  originId, updateWsData, messageTick, saveField, registerReplaceKeys
}) => {
  const seenHeadings = useContext(HeadingContext);  // no repeated titles

  const [formState, setFormState] = useState<Form>({
    title: formName, description: '', fields: []
  });

  const editingFields = useRef(new Set<string>());

  // Shallow merge of every live frame: lets widgets read sibling readState
  // keys that aren't fields themselves (progress maxKey/markerKey, trend refl/vrefl).
  const [liveValues, setLiveValues] = useState<Record<string, any>>({});

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
        // Layout markers (web::Cols / web::Break) apply here too.
        const marker = markerFromRaw(obj, idx);
        if (marker) return { ...marker, baseLabel: marker.label } as unknown as Field & { baseLabel: string };
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

  // Arrays that every push sends whole: category trends and replace-mode
  // tables (the default mode). Registered for this form's lifetime.
  const fullSetKeys = useMemo(() => {
    const keys: string[] = [];
    formState.fields.forEach((f: any) => {
      const o: string = f.o || '';
      const base = f.baseLabel || f.label;
      if (f.type === 'trend' && /(?:^|;)cat(?:;|$)/.test(o)) keys.push(base);
      if (f.type === 'table' && (o.match(/mode=([^;]+)/)?.[1] || 'replace').trim() === 'replace') keys.push(base);
    });
    return keys.sort().join('|');
  }, [formState.fields]);
  useEffect(() => {
    if (!registerReplaceKeys || !fullSetKeys) return;
    return registerReplaceKeys(fullSetKeys.split('|'));
  }, [registerReplaceKeys, fullSetKeys]);

  // WS apply (очікуємо boolean для двійкових)
  const applyWs = useCallback((ws: any) => {
    setFormState((prev) => {
      const upd = prev.fields.map((f: any) => {
        const { label, type, o } = f;
        const base = f.baseLabel || label;

        if (type === 'trend') {
          const inc = getVal(ws, base);
          if (!Array.isArray(inc)) return f;
          // Category trend (cat): every push replaces the rows.
          if (/(?:^|;)cat(?:;|$)/.test(o)) return { ...f, value: inc };
          if (!inc.length) return f;

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

  useEffect(() => {
    if (!wsData) return;
    applyWs(wsData);
    const src = wsData?.status?.fields && typeof wsData.status.fields === 'object' ? wsData.status.fields : wsData;
    if (src && typeof src === 'object' && !Array.isArray(src)) {
      setLiveValues((prev) => ({ ...prev, ...src }));
    }
  }, [wsData, applyWs]);

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
    if (saveField) void saveField({ [lbl]: uiVal });
    else updateWsData({ type: 'p', origin_id: originId, p: { [lbl]: uiVal } }, true);

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
    <SectionContent
      title={visibleHeading(formState.title || 'Status', seenHeadings)}
      titleGutter
      actions={<TabActionsBar liveConnected={connected} messageTick={messageTick} />}
    >
      <DynamicContentHandler
        title={formState.title}
        description={formState.description}
        fields={formState.fields}
        onInputChange={onChange}
        onFieldBlur={onBlur}
        hideTitle
        liveValues={liveValues}
      />
    </SectionContent>
  );
};

export default DynamicStatus;
