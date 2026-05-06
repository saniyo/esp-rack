import { FC, useContext, useMemo } from 'react';
import { Navigate } from "react-router-dom";

import * as AuthenticationApi from '../../api/authentication';
import { AuthenticationContext } from '../../contexts/authentication';
import { RequiredChildrenProps } from '../../utils';
import { FeaturesContext } from '../../contexts/features';
import { useManifest } from '../../contexts/manifest';

const RequireUnauthenticated: FC<RequiredChildrenProps> = ({ children }) => {
  const { features } = useContext(FeaturesContext);
  const { manifest } = useManifest();
  const authenticationContext = useContext(AuthenticationContext);

  // Manifest-aware default landing — first menu-visible feature in
  // manifest order, with the router-match wildcard stripped (Navigate
  // takes a literal URL, the "/system/*" pattern would land us on a
  // URL containing a literal asterisk which matches nothing). Falls
  // back to /system on the empty-manifest edge case so we never feed
  // Navigate '/' (which would bounce right back through this guard
  // and produce a white-screen loop).
  const defaultPath = useMemo(() => {
    const first = manifest.features.find(
      (f) => f.menu?.label && !f.menu?.hidden && f.route
    );
    return (first?.route ?? '/system').replace(/\/\*$/, '');
  }, [manifest]);

  return (
    authenticationContext.me ?
      <Navigate to={AuthenticationApi.fetchLoginRedirect(features, defaultPath)} />
      :
      <>{children}</>
  );

};

export default RequireUnauthenticated;
