import { useCallback, useEffect, useState } from "react";
import { useSnackbar } from 'notistack';
import { AxiosPromise } from "axios";

import { extractErrorMessage } from ".";

export interface RestRequestOptions<D> {
  read: () => AxiosPromise<D>;
  update?: (value: D) => AxiosPromise<D>;
}

export const useRest = <D>({ read, update }: RestRequestOptions<D>) => {
  const { enqueueSnackbar } = useSnackbar();

  const [saving, setSaving] = useState<boolean>(false);
  const [data, setData] = useState<D>();
  const [errorMessage, setErrorMessage] = useState<string>();

  // Silent refetches keep the previous data in place while a new fetch is
  // in flight and swallow errors without a snackbar. Used by the action-
  // triggered refetch schedule (e.g. WiFi scan polling) where transient
  // failures during the scan window are expected and resolve on the
  // next scheduled attempt.
  const loadData = useCallback(async (opts?: { silent?: boolean }) => {
    const silent = !!opts?.silent;
    if (!silent) {
      setData(undefined);
      setErrorMessage(undefined);
    }
    try {
      setData((await read()).data);
      if (silent) setErrorMessage(undefined);
    } catch (error: any) {
      if (silent) return;
      const message = extractErrorMessage(error, 'Problem loading data');
      enqueueSnackbar(message, { variant: 'error' });
      setErrorMessage(message);
    }
  }, [read, enqueueSnackbar]);

  const save = useCallback(async (toSave: D) => {
    if (!update) {
      return;
    }
    setSaving(true);
    setErrorMessage(undefined);
    try {
      setData((await update(toSave)).data);
      enqueueSnackbar("Update successful", { variant: 'success' });
    } catch (error: any) {
      const message = extractErrorMessage(error, 'Problem saving data');
      enqueueSnackbar(message, { variant: 'error' });
      setErrorMessage(message);
    } finally {
      setSaving(false);
    }
  }, [update, enqueueSnackbar]);

  const saveData = () => data && save(data);

  useEffect(() => {
    loadData();
    // Re-fetch on `loadData` identity change. loadData is memoised by
    // useCallback over [read, enqueueSnackbar]; `read` itself is a useMemo
    // keyed on the consumer's own deps (e.g. tab.restPath in
    // DynamicTabPanel), so a tab switch flips the identity and a fresh
    // fetch runs against the new endpoint. Empty-deps was triggering the
    // observed "navigate to second tab → loader spins forever" — the
    // first instance had already mounted and useEffect never refired
    // when React reused the same DOM slot for a different tab.
  }, [loadData]);

  // Self-healing on focus / network resume. Two failure modes covered:
  //   * Browser suspended the tab while in background — coming back, the
  //     last-fetched data may be stale (device rebooted, config changed
  //     in another session). A silent refetch on visibilitychange brings
  //     the page back in sync without flickering an empty form.
  //   * OS-level network outage (Wi-Fi blip, sleep). The `online` event
  //     fires when connectivity returns; immediate refetch surfaces any
  //     state that changed during the gap.
  // Both paths are silent (no spinner, no snackbar) — the data either
  // updates or stays the same. A failure during the silent refetch is
  // ignored; the next user interaction will hit the loud path.
  useEffect(() => {
    const onVisible = () => {
      if (document.visibilityState === 'visible') {
        loadData({ silent: true });
      }
    };
    const onOnline = () => loadData({ silent: true });
    document.addEventListener('visibilitychange', onVisible);
    window.addEventListener('online', onOnline);
    return () => {
      document.removeEventListener('visibilitychange', onVisible);
      window.removeEventListener('online', onOnline);
    };
  }, [loadData]);

  // NOTE: no debug log of `data` here — it fires on every REST fetch and the
  // full response is retained by DevTools indefinitely, inflating tab heap
  // especially on high-cadence live tabs. Re-enable locally when debugging.

  return { loadData, saveData, saving, setData, data, errorMessage } as const;
};
