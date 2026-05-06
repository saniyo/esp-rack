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
