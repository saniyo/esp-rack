import { AxiosPromise } from "axios";
import * as H from 'history';
import jwtDecode from 'jwt-decode';
import { Path } from "react-router-dom";

import { Features, Me, SignInRequest, SignInResponse } from "../types";

import { ACCESS_TOKEN, AXIOS } from "./endpoints";

export const SIGN_IN_PATHNAME = 'loginPathname';
export const SIGN_IN_SEARCH = 'loginSearch';

// Post-login fallback URL when the user didn't trigger sign-in by
// hitting a protected route (i.e. signInPathname not set). MUST NOT
// be '/' — AppRouting maps '/' to SignIn under RequireUnauthenticated,
// so navigating an authenticated user to '/' bounces back through
// RequireUnauthenticated → fetchLoginRedirect('/') → infinite loop
// (presents as a permanent white screen after sign-in).
//
// '/system' is always present (App ctor registers the compound
// feature unconditionally) and AppRouting routes '/system' through
// AuthenticatedRouting, so it terminates the redirect chain cleanly.
// Real callers should usually compute a manifest-aware default at the
// call site and pass it via fetchLoginRedirect's `defaultPath` arg
// (e.g. RequireUnauthenticated does this) — this fallback is for the
// rare path that doesn't have manifest context.
export const getDefaultRoute = (_features: Features) => '/system';

export function verifyAuthorization(): AxiosPromise<void> {
  return AXIOS.get('/verifyAuthorization');
}

export function signIn(request: SignInRequest): AxiosPromise<SignInResponse> {
  return AXIOS.post('/signIn', request);
}

/**
 * Fallback to sessionStorage if localStorage is absent. WebView may not have local storage enabled.
 */
export function getStorage() {
  return localStorage || sessionStorage;
}

export function storeLoginRedirect(location?: H.Location) {
  if (location) {
    getStorage().setItem(SIGN_IN_PATHNAME, location.pathname);
    getStorage().setItem(SIGN_IN_SEARCH, location.search);
  }
}

export function clearLoginRedirect() {
  getStorage().removeItem(SIGN_IN_PATHNAME);
  getStorage().removeItem(SIGN_IN_SEARCH);
}

// `defaultPath` is where the user lands when no signInPathname was
// stashed (clean log-in, not a deep-link bounce). Caller passes a
// manifest-aware path; we keep the legacy getDefaultRoute() fallback
// for rare call sites without manifest context.
export function fetchLoginRedirect(features: Features, defaultPath?: string): Partial<Path> {
  const signInPathname = getStorage().getItem(SIGN_IN_PATHNAME);
  const signInSearch = getStorage().getItem(SIGN_IN_SEARCH);
  clearLoginRedirect();
  return {
    pathname: signInPathname || defaultPath || getDefaultRoute(features),
    search: (signInPathname && signInSearch) || undefined
  };
}

export const clearAccessToken = () => localStorage.removeItem(ACCESS_TOKEN);
export const decodeMeJWT = (accessToken: string): Me => jwtDecode(accessToken) as Me;

export function addAccessTokenParameter(url: string) {
  const accessToken = getStorage().getItem(ACCESS_TOKEN);
  if (!accessToken) {
    return url;
  }
  const parsedUrl = new URL(url);
  parsedUrl.searchParams.set(ACCESS_TOKEN, accessToken);
  return parsedUrl.toString();
}
