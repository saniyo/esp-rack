import { FC, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Navigate, Route, Routes } from 'react-router-dom';
import { AxiosPromise } from 'axios';
import { Tab } from '@mui/material';

import { AXIOS, WEB_SOCKET_ROOT } from '../../api/endpoints';
import { RouterTabs, useRouterTab, useLayoutTitle } from '../../components';
import DynamicStatus from './DynamicStatus';
import DynamicSettings from './DynamicSettings';
import FormLoader from '../loading/FormLoader';
import { useManifest } from '../../contexts/manifest';
import { FeatureEntry, TabSpec } from '../../types';
import { useRest } from '../../utils';
import { useWs } from '../../utils/useWs';
import { DynamicFormContext, DynamicFormCtx } from './DynamicFormContext';

// AXIOS baseURL is '/rest/'. Manifest publishes absolute paths; strip the
// '/rest' prefix before handing to axios so we don't end up with a
// double-prefixed URL that the server rewrites into the SPA fallback.
const toAxiosPath = (absPath: string) =>
  absPath.startsWith('/rest/') ? absPath.slice(5) : absPath;

// Per-tab panel. Owns its own REST fetch keyed by tab.restPath — crucial
// for features whose tabs live on different endpoints (e.g. 'system' with
// /rest/system/status + /rest/ota + /rest/upload).
interface DynamicTabPanelProps {
  tab: TabSpec;
  ws?: {
    connected: boolean;
    wsData: any;
    originId: string;
    updateWsData: any;
    disconnect: () => void;
  };
}

const DynamicTabPanel: FC<DynamicTabPanelProps> = ({ tab, ws }) => {
  const read = useMemo(
    () => (): AxiosPromise<Record<string, any>> => AXIOS.get(toAxiosPath(tab.restPath)),
    [tab.restPath]
  );
  const update = useMemo(
    () =>
      tab.postable
        ? (d: Record<string, any>): AxiosPromise<Record<string, any>> =>
            AXIOS.post(toAxiosPath(tab.restPath), d)
        : undefined,
    [tab.restPath, tab.postable]
  );

  const { data, saveData, saving, setData, errorMessage, loadData } = useRest<any>({ read, update });

  // --- Form-level context shared by descendants (ActionField refetch,
  // TableField loader, …). Pending-action tracking is a tiny scheduler:
  // markPending(id, ms) flips isPending(id) to true, setTimeout clears it.
  // A bumper state var forces consumers (TableField) to re-render when
  // the pending set changes without exposing the mutable Set directly.
  const pendingRef = useRef<Map<string, number>>(new Map());
  const [pendingTick, setPendingTick] = useState(0);

  const markPending = useCallback((id: string, durationMs: number) => {
    const prev = pendingRef.current.get(id);
    if (prev) window.clearTimeout(prev);
    const t = window.setTimeout(() => {
      pendingRef.current.delete(id);
      setPendingTick((n) => n + 1);
    }, durationMs);
    pendingRef.current.set(id, t as unknown as number);
    setPendingTick((n) => n + 1);
  }, []);

  const clearPending = useCallback((id: string) => {
    const prev = pendingRef.current.get(id);
    if (prev) window.clearTimeout(prev);
    if (pendingRef.current.delete(id)) setPendingTick((n) => n + 1);
  }, []);

  const isPending = useCallback((id: string) => pendingRef.current.has(id), [pendingTick]);

  // The response may wrap the form under tab.key ({ [tab.key]: {description,
  // fields} }) or be flat ({ description, fields }). Try both.
  const formData = useMemo(() => {
    if (!data) return undefined;
    if (typeof data !== 'object' || Array.isArray(data)) return undefined;
    if (tab.key in data && typeof (data as any)[tab.key] === 'object') {
      return (data as any)[tab.key];
    }
    if (Array.isArray((data as any).fields)) return data;
    return undefined;
  }, [data, tab.key]);

  // Form state surfaced to descendants so ActionField / TableField can
  // evaluate *If-style tags (loadingIf, busyIf) against sibling fields
  // without prop drilling. Built by flattening the current form envelope.
  const flattenedFormState = useMemo<Record<string, any>>(() => {
    const out: Record<string, any> = {};
    if (!formData || !Array.isArray((formData as any).fields)) return out;
    for (const raw of (formData as any).fields as Array<Record<string, any>>) {
      const key = Object.keys(raw).find((k) => k !== 'o');
      if (key) out[key] = (raw as any)[key];
    }
    return out;
  }, [formData]);

  const ctx: DynamicFormCtx = useMemo(() => ({
    refetch: (opts) => { void loadData(opts); },
    isPending,
    markPending,
    clearPending,
    formState: flattenedFormState,
  }), [loadData, isPending, markPending, clearPending, flattenedFormState]);

  const handleUpdateData = useCallback(
    (newData: Partial<any>) => {
      setData((prev: any) => {
        const prevTabData = (prev && prev[tab.key]) || prev || {};
        return {
          ...(prev ?? {}),
          [tab.key]: { ...prevTabData, ...newData },
        };
      });
    },
    [setData, tab.key]
  );

  const handleSaveData = useCallback(
    (updated: Partial<any>) => {
      if (!update) return;
      update(updated)
        .then(() => saveData())
        .catch((err) => console.error('Error saving data:', err));
    },
    [update, saveData]
  );

  if (!formData || !Array.isArray((formData as any).fields)) {
    return <FormLoader message={`Loading ${tab.title ?? tab.key}…`} errorMessage={errorMessage} />;
  }

  if (tab.live && ws) {
    return (
      <DynamicFormContext.Provider value={ctx}>
        <DynamicStatus
          formName={tab.title ?? tab.key}
          data={formData}
          wsData={ws.wsData}
          connected={ws.connected}
          saving={saving}
          originId={ws.originId}
          setData={handleUpdateData}
          disconnect={ws.disconnect}
          updateWsData={ws.updateWsData}
        />
      </DynamicFormContext.Provider>
    );
  }

  return (
    <DynamicFormContext.Provider value={ctx}>
      <DynamicSettings
        formName={tab.title ?? tab.key}
        data={formData}
        saveData={tab.postable ? handleSaveData : () => {}}
        saving={saving}
        setData={handleUpdateData}
        readOnly={!tab.postable}
      />
    </DynamicFormContext.Provider>
  );
};

// Top-level manifest-driven feature page.
//
// C++ declares any number of tabs in WebFeatureSpec — each with its own
// restPath (which endpoint to fetch) and interaction mode (postable writes
// back, live subscribes to the feature's WS). Each backend reader uses
// FormBuilder::createForm(root, "<tab.key>", …) to populate that tab's
// form content; this component just iterates the manifest and renders.
interface DynamicFeatureProps {
  featureId: string;
  fallback?: FeatureEntry;
}

const DynamicFeature: FC<DynamicFeatureProps> = ({ featureId, fallback }) => {
  const { findFeature } = useManifest();
  const entry = findFeature(featureId) ?? fallback;

  useLayoutTitle(entry?.title ?? featureId);
  const { routerTab } = useRouterTab();

  const wsUrl = useMemo<string | undefined>(() => {
    if (!entry?.ws) return undefined;
    return WEB_SOCKET_ROOT + entry.ws.replace(/^\/ws\//, '');
  }, [entry?.ws]);

  const { connected, wsData, updateData: updateWsData, disconnect } = useWs<any>(wsUrl ?? '');

  const [originId, setOriginId] = useState<string>('');
  useEffect(() => {
    if (wsData && wsData.origin_id) setOriginId(wsData.origin_id);
  }, [wsData]);

  const wsBundle = wsUrl
    ? { connected, wsData, originId, updateWsData, disconnect }
    : undefined;

  if (!entry) return <FormLoader message="Loading feature…" />;

  const tabs = entry.tabs ?? [];
  if (tabs.length === 0) return <FormLoader message="Feature has no tabs" />;
  const firstTabKey = tabs[0].key;

  if (tabs.length === 1) {
    return <DynamicTabPanel tab={tabs[0]} ws={wsBundle} />;
  }

  return (
    <>
      <RouterTabs value={routerTab}>
        {tabs.map((t) => (
          <Tab key={t.key} value={t.key} label={t.title ?? t.key} />
        ))}
      </RouterTabs>
      <Routes>
        {tabs.map((t) => (
          <Route
            key={t.key}
            path={t.key}
            element={<DynamicTabPanel tab={t} ws={wsBundle} />}
          />
        ))}
        <Route path="/*" element={<Navigate replace to={firstTabKey} />} />
      </Routes>
    </>
  );
};

export default DynamicFeature;
