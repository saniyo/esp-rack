import { mergeSchemaState, staticStatePath } from './utils/formSchema';
import { HeadingContext } from './HeadingContext';
import { FC, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Navigate, Route, Routes } from 'react-router-dom';
import { AxiosPromise } from 'axios';
import { Tab } from '@mui/material';

import { AXIOS, toAxiosPath } from '../../api/endpoints';
import { RouterTabs, useRouterTab, useLayoutTitle } from '../../components';
import DynamicStatus from './DynamicStatus';
import DynamicSettings from './DynamicSettings';
import FormLoader from '../loading/FormLoader';
import Alert from '@mui/material/Alert';
import { useManifest } from '../../contexts/manifest';
import { extHwModules, usedByLabel } from '../layout/extHwMenu';
import { FeatureEntry, TabSpec } from '../../types';
import { useRest } from '../../utils';
import { useWsChannel } from '../../utils/useWsChannel';
import { DynamicFormContext, DynamicFormCtx } from './DynamicFormContext';


// C1 path contract: a typed feature serves its UI form at /api/v1/<id>/form
// (GET only) and its write face at /api/v1/<id>/state (POST). Postable tabs
// always reference a typed feature's form — their own, or another one via
// formOf — so the save target is that form path with /form -> /state.
// Bespoke paths (no /form suffix) keep posting where they read.
const toSavePath = (formPath: string) => formPath.replace(/\/form$/, '/state');

// Per-tab panel. Owns its own REST fetch keyed by tab.restPath — crucial
// for features whose tabs live on different endpoints (e.g. 'system' with
// /api/v1/system/status/form + /api/v1/ota/form + /api/v1/system/upload/form).
interface DynamicTabPanelProps {
  tab: TabSpec;
  // SES-LIB-033: a static-schema tab: cached <restPath>?schema=1 + the values
  // from this state path instead of the full form.
  statePath?: string;
  ws?: {
    connected: boolean;
    wsData: any;
    originId: string;
    updateWsData: any;
    disconnect: () => void;
    // Counter that bumps on every inbound WS frame. Drives the
    // LiveIndicator's per-message pulse animation — see TabActionsBar.
    messageTick: number;
    // Keys whose arrays are full sets, for coalescing (useWsChannel).
    registerReplaceKeys: (keys: string[]) => () => void;
  };
}

const DynamicTabPanel: FC<DynamicTabPanelProps> = ({ tab, ws, statePath }) => {
  const read = useMemo(
    () => (): AxiosPromise<Record<string, any>> => {
      if (!statePath) return AXIOS.get(toAxiosPath(tab.restPath));
      return Promise.all([
        AXIOS.get(`${toAxiosPath(tab.restPath)}?schema=1`),
        AXIOS.get(toAxiosPath(statePath)),
      ]).then(([schema, state]) => ({ ...schema, data: mergeSchemaState(schema.data, state.data) }));
    },
    [tab.restPath, statePath]
  );
  const update = useMemo(
    () =>
      tab.postable
        ? (d: Record<string, any>): AxiosPromise<Record<string, any>> =>
            AXIOS.post(toAxiosPath(toSavePath(tab.restPath)), d)
        : undefined,
    [tab.restPath, tab.postable]
  );

  // The state POST echoes flat values, not this tab's form envelope —
  // re-read the form after a save instead of adopting the echo.
  const { data, save, saving, setData, errorMessage, loadData } =
    useRest<any>({ read, update, refetchOnSave: true });

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

  // Live-edit ref backing the context's getLiveValue/setLiveValue
  // pair. DynamicContentHandler writes into this on every
  // handleInputChange (typing into a TextField, rowFill from a
  // TableField, etc.); ActionField's `withFields` reads from it at
  // click time. We use a ref + stable getter/setter pair (instead of
  // a state map in the context value) so per-keystroke edits don't
  // create a new ctx object and force every consumer to re-render.
  const liveValuesRef = useRef<Record<string, any>>({});
  const setLiveValue = useCallback((label: string, value: any) => {
    liveValuesRef.current[label] = value;
  }, []);
  const getLiveValue = useCallback((label: string) => liveValuesRef.current[label], []);

  const ctx: DynamicFormCtx = useMemo(() => ({
    refetch: (opts) => { void loadData(opts); },
    isPending,
    markPending,
    clearPending,
    formState: flattenedFormState,
    getLiveValue,
    setLiveValue,
  }), [loadData, isPending, markPending, clearPending, flattenedFormState, getLiveValue, setLiveValue]);

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

  // One POST of the changed keys only (DynamicSettings sends a partial flat
  // dict), then a silent form refetch. The old path posted the changed keys
  // and then the whole form envelope a second time.
  const handleSaveData = useCallback(
    (updated: Partial<any>) => {
      if (!update) return;
      void save(updated);
    },
    [update, save]
  );

  // A field committed on a live, postable tab goes through the same REST
  // write as a Settings save: the device answers, a refusal shows up as a
  // snackbar and the form is read again so the field shows the device's
  // value. (Over the socket a write had no answer: a refused or lost one
  // left the field showing a value the device didn't take.)
  const handleSaveLiveField = useCallback(
    async (patch: Record<string, any>) => {
      if (!update) return;
      if (!(await save(patch))) void loadData({ silent: true });
    },
    [update, save, loadData]
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
          saveField={tab.postable ? handleSaveLiveField : undefined}
          registerReplaceKeys={ws.registerReplaceKeys}
          messageTick={ws.messageTick}
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
  const { findFeature, manifest, ensureActions } = useManifest();
  const entry = findFeature(featureId) ?? fallback;
  useEffect(() => {
    ensureActions(featureId);  // its own actions (SES-LIB-030)
  }, [featureId, ensureActions]);
  // A page of an owned external HW instance says who uses it (SES-LIB-028).
  const usedBy = useMemo(
    () => (entry ? usedByLabel(extHwModules(manifest), entry, manifest.features) : undefined),
    [entry, manifest],
  );
  const banner = usedBy ? <Alert severity="info" sx={{ mb: 2 }}>Used by {usedBy}</Alert> : null;

  useLayoutTitle(entry?.title ?? featureId);
  const { routerTab } = useRouterTab();

  // Live features advertise a CHANNEL id, multiplexed over the single /ws/app
  // socket (see WsProvider). `ws` is the legacy per-feature socket path, still
  // honoured as a fallback by stripping the /ws/ prefix.
  const channel = useMemo<string | undefined>(() => {
    if (entry?.ch) return entry.ch;
    if (entry?.ws) return entry.ws.replace(/^\/ws\//, '');
    return undefined;
  }, [entry?.ch, entry?.ws]);

  const { connected, wsData, updateData: updateWsData, disconnect, messageTick, registerReplaceKeys } =
    useWsChannel<any>(channel);

  const [originId, setOriginId] = useState<string>('');
  useEffect(() => {
    if (wsData && wsData.origin_id) setOriginId(wsData.origin_id);
  }, [wsData]);

  const wsBundle = channel
    ? { connected, wsData, originId, updateWsData, disconnect, messageTick, registerReplaceKeys }
    : undefined;

  if (!entry) return <FormLoader message="Loading feature…" />;

  const tabs = entry.tabs ?? [];
  if (tabs.length === 0) return <FormLoader message="Feature has no tabs" />;
  const firstTabKey = tabs[0].key;

  // SES-LIB-033: static-schema tabs read the cached schema + their state.
  const statePathOf = (t: TabSpec) => staticStatePath(entry, t);

  // Titles the page already shows; forms and cards below don't repeat them.
  const shown = (t: { title?: string; key: string }) => [entry.title, entry.menu?.label, t.title, t.key];

  if (tabs.length === 1) {
    return (
      <>
        {banner}
        <HeadingContext.Provider value={shown(tabs[0])}>
          <DynamicTabPanel tab={tabs[0]} ws={wsBundle} statePath={statePathOf(tabs[0])} />
        </HeadingContext.Provider>
      </>
    );
  }

  return (
    <>
      {banner}
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
            element={(
              <HeadingContext.Provider value={shown(t)}>
                <DynamicTabPanel tab={t} ws={wsBundle} statePath={statePathOf(t)} />
              </HeadingContext.Provider>
            )}
          />
        ))}
        <Route path="/*" element={<Navigate replace to={firstTabKey} />} />
      </Routes>
    </>
  );
};

export default DynamicFeature;
