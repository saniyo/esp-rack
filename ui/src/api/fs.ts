import { AxiosPromise, AxiosRequestConfig } from 'axios';
import { AXIOS } from './endpoints';

// Paths keyed by role. Defaults mirror the canonical routes; when the
// UI manifest is loaded, configureFsEndpoints() overwrites any roles the
// backend publishes. AXIOS baseURL is '/api/v1/', hence the '/api/v1/fs/…'
// absolute form for direct URL strings.
type FsRole =
  | 'schema' | 'volumes' | 'list' | 'stat'
  | 'download' | 'upload'
  | 'mkdir' | 'delete' | 'rename'
  | 'format' | 'formatStatus'
  | 'unmount' | 'mount'
  | 'archivePrepare' | 'archiveStatus' | 'archiveDownload' | 'archiveCancel'
  | 'hashStart' | 'hashStatus' | 'hashCancel';

const FS_PATHS: Record<FsRole, string> = {
  schema: '/api/v1/fs/schema',
  volumes: '/api/v1/fs/volumes',
  list: '/api/v1/fs/list',
  stat: '/api/v1/fs/stat',
  download: '/api/v1/fs/download',
  upload: '/api/v1/fs/upload',
  mkdir: '/api/v1/fs/mkdir',
  delete: '/api/v1/fs/delete',
  rename: '/api/v1/fs/rename',
  format: '/api/v1/fs/format',
  formatStatus: '/api/v1/fs/format/status',
  unmount: '/api/v1/fs/unmount',
  mount: '/api/v1/fs/mount',
  archivePrepare: '/api/v1/fs/archive/prepare',
  archiveStatus: '/api/v1/fs/archive/status',
  archiveDownload: '/api/v1/fs/archive/download',
  archiveCancel: '/api/v1/fs/archive/cancel',
  hashStart: '/api/v1/fs/hash/start',
  hashStatus: '/api/v1/fs/hash/status',
  hashCancel: '/api/v1/fs/hash/cancel',
};

export interface ManifestFsEndpoint {
  method?: string;
  path?: string;
  role?: string;
}

export function configureFsEndpoints(endpoints: ManifestFsEndpoint[] | undefined): void {
  if (!endpoints) return;
  endpoints.forEach((e) => {
    if (e.role && e.path && (e.role as FsRole) in FS_PATHS) {
      FS_PATHS[e.role as FsRole] = e.path;
    }
  });
}

// AXIOS uses baseURL='/api/v1/'; strip the '/api/v1' prefix before passing to axios.
const toAxios = (absPath: string) =>
  absPath.startsWith('/api/v1/') ? absPath.slice(7) : absPath;

export interface FsVolume {
  name: string;
  mount: string;
  mounted: boolean;
  readOnly: boolean;
  canFormat: boolean;
  total: number;
  used: number;
}

export interface FsEntry {
  name: string;
  isDir: boolean;
  size: number;
  mtime: number;
}

export interface FsListResponse {
  path: string;
  readOnly: boolean;
  entries: FsEntry[];
}

export function listVolumes(): AxiosPromise<FsVolume[]> {
  return AXIOS.get(toAxios(FS_PATHS.volumes));
}

export function listDir(path: string): AxiosPromise<FsListResponse> {
  return AXIOS.get(toAxios(FS_PATHS.list), { params: { path } });
}

export function stat(path: string): AxiosPromise<FsEntry> {
  return AXIOS.get(toAxios(FS_PATHS.stat), { params: { path } });
}

export function downloadUrl(path: string): string {
  return `${FS_PATHS.download}?path=${encodeURIComponent(path)}`;
}

export interface UploadOpts {
  sha256?: string;
  crc32?: string;
}

export function uploadFile(
  targetDir: string,
  file: File,
  onUploadProgress?: (ev: ProgressEvent) => void,
  cancelToken?: AxiosRequestConfig['cancelToken'],
  opts?: UploadOpts
): AxiosPromise<{ ok: boolean }> {
  const form = new FormData();
  form.append('file', file, file.name);
  const headers: Record<string, string> = { 'Content-Type': 'multipart/form-data' };
  if (opts?.sha256) headers['X-File-SHA256'] = opts.sha256;
  if (opts?.crc32) headers['X-File-CRC32'] = opts.crc32;
  return AXIOS.post(toAxios(FS_PATHS.upload), form, {
    params: { path: targetDir },
    headers,
    onUploadProgress: onUploadProgress as any,
    cancelToken,
  });
}

export function mkdir(path: string): AxiosPromise<{ ok: boolean }> {
  return AXIOS.post(toAxios(FS_PATHS.mkdir), { path });
}

export function remove(path: string, recursive = false): AxiosPromise<{ ok: boolean }> {
  return AXIOS.post(toAxios(FS_PATHS.delete), { path, recursive });
}

export function rename(from: string, to: string): AxiosPromise<{ ok: boolean }> {
  return AXIOS.post(toAxios(FS_PATHS.rename), { from, to });
}

export function fetchSchema(): AxiosPromise<any> {
  return AXIOS.get(toAxios(FS_PATHS.schema));
}

export function formatVolume(volume: string): AxiosPromise<{ ok: boolean }> {
  return AXIOS.post(toAxios(FS_PATHS.format), { volume, confirm: true });
}

export interface FormatStatus {
  state: 'idle' | 'counting' | 'running' | 'done' | 'error';
  percent: number;
  total: number;
  done: number;
}

export function formatStatus(volume: string): AxiosPromise<FormatStatus> {
  return AXIOS.get(toAxios(FS_PATHS.formatStatus), { params: { volume } });
}

export type ArchiveState = 'idle' | 'walking' | 'ready' | 'streaming' | 'error';

export interface ArchiveStatus {
  state: ArchiveState;
  walked: number;
  totalBytes: number;
  entries: number;
  path: string;
  archiveName: string;
  error?: string;
}

export function archivePrepare(path: string): AxiosPromise<{ ok: boolean; status?: string }> {
  return AXIOS.post(toAxios(FS_PATHS.archivePrepare), { path });
}

export function archiveStatus(): AxiosPromise<ArchiveStatus> {
  return AXIOS.get(toAxios(FS_PATHS.archiveStatus));
}

export function archiveDownloadUrl(): string {
  return FS_PATHS.archiveDownload;
}

export function archiveCancel(): AxiosPromise<{ ok: boolean }> {
  return AXIOS.post(toAxios(FS_PATHS.archiveCancel), {});
}

export function unmountVolume(volume: string): AxiosPromise<{ ok: boolean }> {
  return AXIOS.post(toAxios(FS_PATHS.unmount), { volume });
}

export function mountVolume(volume: string): AxiosPromise<{ ok: boolean }> {
  return AXIOS.post(toAxios(FS_PATHS.mount), { volume });
}

export function readText(path: string): AxiosPromise<string> {
  return AXIOS.get(toAxios(FS_PATHS.download), {
    params: { path },
    responseType: 'text',
    transformResponse: [(data) => data],
  });
}

export type HashState = 'idle' | 'running' | 'done' | 'error';

export interface HashStatus {
  state: HashState;
  bytesRead: number;
  total: number;
  percent: number;
  path: string;
  sha256?: string;
  crc32?: string;
  error?: string;
}

export function hashStart(path: string): AxiosPromise<{ ok: boolean; status?: string }> {
  return AXIOS.post(toAxios(FS_PATHS.hashStart), { path });
}

export function hashStatus(): AxiosPromise<HashStatus> {
  return AXIOS.get(toAxios(FS_PATHS.hashStatus));
}

export function hashCancel(): AxiosPromise<{ ok: boolean }> {
  return AXIOS.post(toAxios(FS_PATHS.hashCancel), {});
}

export function writeText(path: string, content: string): AxiosPromise<{ ok: boolean }> {
  const slash = path.lastIndexOf('/');
  const dir = slash > 0 ? path.slice(0, slash) : '/';
  const name = slash >= 0 ? path.slice(slash + 1) : path;
  const form = new FormData();
  form.append('file', new Blob([content], { type: 'text/plain' }), name);
  return AXIOS.post(toAxios(FS_PATHS.upload), form, {
    params: { path: dir },
    headers: { 'Content-Type': 'multipart/form-data' },
  });
}
