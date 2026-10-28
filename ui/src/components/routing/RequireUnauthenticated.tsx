import { FC, useContext, useMemo } from 'react';
import { Navigate } from "react-router-dom";

import * as AuthenticationApi from '../../api/authentication';
import { AuthenticationContext } from '../../contexts/authentication';
import { RequiredChildrenProps } from '../../utils';
import { FeaturesContext } from '../../contexts/features';
import { useManifest } from '../../contexts/manifest';
import { startPath } from '../../utils/startPage';

const RequireUnauthenticated: FC<RequiredChildrenProps> = ({ children }) => {
  const { features } = useContext(FeaturesContext);
  const { manifest } = useManifest();
  const authenticationContext = useContext(AuthenticationContext);

  // The start page, the same everywhere (utils/startPage): the landing
  // feature, else the role feature, else the first menu item. Never '/'
  // (it would bounce right back through this guard): /system when the
  // manifest is empty.
  const defaultPath = useMemo(() => startPath(manifest.features), [manifest]);

  return (
    authenticationContext.me ?
      <Navigate to={AuthenticationApi.fetchLoginRedirect(features, defaultPath)} />
      :
      <>{children}</>
  );

};

export default RequireUnauthenticated;
