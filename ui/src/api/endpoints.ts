import axios, { AxiosPromise, CancelToken } from 'axios';

import { getClientKey } from '../utils/clientKey';

export const WS_BASE_URL = '/ws/';
export const API_BASE_URL = '/rest/';
export const ACCESS_TOKEN = 'access_token';
export const WEB_SOCKET_ROOT = calculateWebSocketRoot(WS_BASE_URL);

export const AXIOS = axios.create({
  baseURL: API_BASE_URL,
  headers: {
    'Content-Type': 'application/json',
  },
  transformRequest: [(data, headers) => {
    if (headers) {
      if (localStorage.getItem(ACCESS_TOKEN)) {
        headers.Authorization = 'Bearer ' + localStorage.getItem(ACCESS_TOKEN);
      }
      if (headers['Content-Type'] !== 'application/json') {
        return data;
      }
    }
    return JSON.stringify(data);
  }]
});

// Transient heap-pressure retry. No-PSRAM devices (ESP32-C3/C6) answer
// 503 {"error":"heap_pressure","retryAfter":N} when the largest free
// heap block is momentarily smaller than a response buffer — typically
// because ANOTHER request from the same page load is mid-flight. That's
// a collision, not an outage: the loser succeeds milliseconds later.
// Retry up to twice with a short backoff before letting the error
// surface as a toast; anything other than 503 passes through untouched.
const RETRY_503_MAX = 2;
const RETRY_503_DELAY_MS = 400;
AXIOS.interceptors.response.use(undefined, (error) => {
  const cfg = error?.config as (typeof error.config & { _retry503?: number }) | undefined;
  const status = error?.response?.status;
  if (!cfg || status !== 503) return Promise.reject(error);
  const attempt: number = (cfg._retry503 ?? 0) + 1;
  if (attempt > RETRY_503_MAX) return Promise.reject(error);
  cfg._retry503 = attempt;
  // Server hint (seconds) wins when present; cap it so a misbehaving
  // value can't park the UI for long.
  const hinted = Number(error?.response?.data?.retryAfter);
  const delayMs = Number.isFinite(hinted) && hinted > 0
    ? Math.min(hinted * 1000, 3000)
    : RETRY_503_DELAY_MS * attempt;
  return new Promise((resolve) => setTimeout(resolve, delayMs))
    .then(() => AXIOS.request(cfg));
});

// Presence tracking via REST — every API call carries the stable tab UUID
// and the page the SPA is currently on. Backend's PresenceService reads
// these headers (see SecurityManager::touchPresence) to maintain a live
// registry of every authenticated tab regardless of whether that tab has
// any WebSocket open. Works as the primary identity channel; the
// /ws/presence WebSocket is a real-time enhancement, not a requirement.
AXIOS.interceptors.request.use((config) => {
  const headers = config.headers ?? {};
  headers['X-Client-Key'] = getClientKey();
  try {
    headers['X-Current-Page'] = window.location.pathname + window.location.search;
  } catch {
    // SSR / tests without window — just omit the page header.
  }
  config.headers = headers;
  return config;
});

function calculateWebSocketRoot(webSocketPath: string) {
  const location = window.location;
  const webProtocol = location.protocol === "https:" ? "wss:" : "ws:";
  return webProtocol + "//" + location.host + webSocketPath;
}

export interface FileUploadConfig {
  cancelToken?: CancelToken;
  onUploadProgress?: (progressEvent: ProgressEvent) => void;
}

export const uploadFile = (url: string, file: File, config?: FileUploadConfig): AxiosPromise<void> => {
  const formData = new FormData();
  formData.append('file', file);

  return AXIOS.post(url, formData, {
    headers: {
      'Content-Type': 'multipart/form-data'
    },
    ...(config || {})
  });
};
