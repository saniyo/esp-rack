import { FC, useCallback, useContext, useEffect, useState } from 'react';
import { useSnackbar } from 'notistack';
import { useNavigate } from 'react-router-dom';

import * as AuthenticationApi from '../../api/authentication';
import { ACCESS_TOKEN } from '../../api/endpoints';
import { RequiredChildrenProps } from '../../utils';
import { LoadingSpinner } from '../../components';
import { Me } from '../../types';

import { FeaturesContext } from '../features';
import { useManifest } from '../manifest';
import { AuthenticationContext } from './context';

const Authentication: FC<RequiredChildrenProps> = ({ children }) => {
  const { features } = useContext(FeaturesContext);
  const { reload: reloadManifest } = useManifest();
  const navigate = useNavigate();
  const { enqueueSnackbar } = useSnackbar();

  const [initialized, setInitialized] = useState<boolean>(false);
  const [me, setMe] = useState<Me>();

  const signIn = (accessToken: string) => {
    try {
      AuthenticationApi.getStorage().setItem(ACCESS_TOKEN, accessToken);
      const decodedMe = AuthenticationApi.decodeMeJWT(accessToken);
      setMe(decodedMe);
      enqueueSnackbar(`Logged in as ${decodedMe.username}`, { variant: 'success' });
      // Token now in localStorage → axios attaches it on every request.
      // Re-fetch the manifest so the route table + module list flip
      // from the anonymous stub to the full authenticated payload.
      // Awaited fire-and-forget — no need to block the UI on it; the
      // routes that depend on full content render once state lands.
      void reloadManifest();
    } catch (error: any) {
      setMe(undefined);
      throw new Error("Failed to parse JWT " + error.message);
    }
  };

  const signOut = (redirect: boolean) => {
    AuthenticationApi.clearAccessToken();
    setMe(undefined);
    // Token cleared → manifest backend will now return the anonymous
    // stub. Re-fetch so the in-memory state matches what we'd get on
    // a fresh page load — prevents stale route entries lingering after
    // logout.
    void reloadManifest();
    if (redirect) {
      navigate('/');
    }
  };

  const refresh = useCallback(async () => {
    if (!features.security) {
      setMe({ admin: true, username: "admin" });
      setInitialized(true);
      return;
    }
    const accessToken = AuthenticationApi.getStorage().getItem(ACCESS_TOKEN);
    if (accessToken) {
      try {
        await AuthenticationApi.verifyAuthorization();
        setMe(AuthenticationApi.decodeMeJWT(accessToken));
        setInitialized(true);
      } catch (error: any) {
        // Token failed to verify — typically because the device's
        // JWT secret regenerated on a fresh boot, so a token cached
        // from a previous session can no longer be validated. Wipe
        // it so AXIOS stops sending a dead `Bearer …` on every
        // subsequent request (which was making the server treat the
        // browser as anonymous without the user realising) AND
        // re-fetch the manifest so the route/feature tables match
        // the now-effectively-signed-out state.
        AuthenticationApi.clearAccessToken();
        setMe(undefined);
        setInitialized(true);
        void reloadManifest();
        enqueueSnackbar('Session expired, please sign in again.',
                        { variant: 'info' });
      }
    } else {
      setMe(undefined);
      setInitialized(true);
    }
  }, [features, reloadManifest, enqueueSnackbar]);

  useEffect(() => {
    refresh();
  }, [refresh]);

  if (initialized) {
    return (
      <AuthenticationContext.Provider
        value={{
          signIn,
          signOut,
          me,
          refresh
        }}
      >
        {children}
      </AuthenticationContext.Provider >
    );
  }

  return (
    <LoadingSpinner height="100vh" />
  );
};

export default Authentication;
