import { FC, ReactNode, useCallback, useEffect, useMemo } from 'react';
import { Navigate, Routes, Route, useNavigate, useLocation } from 'react-router-dom';
import { AxiosError } from 'axios';

import PresenceProvider from './contexts/presence/PresenceProvider';
import * as AuthenticationApi from './api/authentication';
import { AXIOS } from './api/endpoints';
import { Layout, RequireAdmin } from './components';
import { useManifest } from './contexts/manifest';
import { FeatureEntry } from './types';

import Security from './framework/security/Security';
import FileSystem from './framework/filesystem/FileSystem';
import DynamicFeature from './components/dynamic-component/DynamicFeature';

// ── Component registry ────────────────────────────────────────────────
//
// Manifest entries carry a `component` string telling the frontend which
// React renderer to use. The vast majority resolve to DynamicFeature —
// the manifest's tabs + FormBuilder JSON drive the entire UI dynamically,
// so adding a new C++ service requires zero TypeScript changes.
//
// A handful of services need bespoke React because their UX exceeds what
// FormBuilder describes (file browser, user CRUD with PBKDF2 lifecycle):
// they ship their renderer here as part of the framework, not as
// consumer custom code. Anyone adding a new bespoke renderer adds it to
// this map; everything else renders through DynamicFeature for free.
//
// The 'security' standalone page (user editor) does not have a manifest
// entry today — it's published directly by the library at
// `/security/*`. Kept as an explicit route alongside the
// manifest-emitted ones so consumers don't lose it if they disable a
// security manifest contribution.
const CUSTOM_RENDERERS: Record<string, FC> = {
  FileSystem,
};

// Render the React node for a manifest entry: bespoke renderer if one
// exists for `component`, DynamicFeature otherwise. Auth-gating happens
// here too — entries flagged Admin get wrapped in <RequireAdmin>.
const renderEntry = (entry: FeatureEntry): ReactNode => {
  const Custom = entry.component ? CUSTOM_RENDERERS[entry.component] : undefined;
  let node: ReactNode = Custom ? <Custom /> : <DynamicFeature featureId={entry.id} />;
  const wantsAdmin = entry.auth === 'admin' || entry.menu?.auth === 'admin';
  if (wantsAdmin) node = <RequireAdmin>{node}</RequireAdmin>;
  return node;
};

// Pull the path that React Router should match from the manifest's
// `route` field. Manifest emits routes like "/wifi/*" (compound feature
// shells with sub-tabs) or "/lightState/*" (auto-derived from id when
// the C++ side didn't set a routeTemplate). React Router wants the
// leading slash dropped because we mount inside <Routes> already.
const pathOf = (entry: FeatureEntry): string | null => {
  if (!entry.route) return null;
  return entry.route.startsWith('/') ? entry.route.slice(1) : entry.route;
};

const AuthenticatedRouting: FC = () => {
  const { manifest } = useManifest();
  const location = useLocation();
  const navigate = useNavigate();

  const handleApiResponseError = useCallback((error: AxiosError) => {
    if (error.response && error.response.status === 401) {
      AuthenticationApi.storeLoginRedirect(location);
      navigate("/unauthorized");
    }
    return Promise.reject(error);
  }, [location, navigate]);

  useEffect(() => {
    const axiosHandlerId = AXIOS.interceptors.response.use((response) => response, handleApiResponseError);
    return () => AXIOS.interceptors.response.eject(axiosHandlerId);
  }, [handleApiResponseError]);

  // Manifest-driven routes. Every entry that has a route + a menu (or a
  // bespoke renderer) gets a matching <Route>. Entries without a route
  // (action-only, hidden helper features) are skipped — they're either
  // navigated to by sibling features programmatically or surfaced
  // through actions, not by URL.
  const manifestRoutes = useMemo(() => {
    return manifest.features
      .map((f) => ({ entry: f, path: pathOf(f) }))
      .filter(({ path }) => !!path)
      .map(({ entry, path }) => (
        <Route key={entry.id} path={path!} element={renderEntry(entry)} />
      ));
  }, [manifest]);

  // Default landing — first menu-visible feature in manifest order.
  // Falls back to /system on the rare manifest-empty bootstrap so
  // RootRedirect lands somewhere instead of looping at "/".
  const defaultPath = useMemo(() => {
    const first = manifest.features.find(
      (f) => f.menu?.label && !f.menu?.hidden && f.route
    );
    return first?.route ?? '/system';
  }, [manifest]);

  return (
    <PresenceProvider>
    <Layout>
      <Routes>
        {manifestRoutes}
        {/* Standalone Security page (user CRUD) — not in manifest today. */}
        <Route
          path="/security/*"
          element={
            <RequireAdmin>
              <Security />
            </RequireAdmin>
          }
        />
        <Route path="/*" element={<Navigate to={defaultPath} />} />
      </Routes>
    </Layout>
    </PresenceProvider>
  );
};

export default AuthenticatedRouting;
