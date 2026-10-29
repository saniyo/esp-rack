import { AxiosPromise, AxiosRequestConfig } from 'axios';
import { AXIOS, toAxiosPath } from './endpoints';

// Paths keyed by role. Defaults mirror the canonical routes; when the
// UI manifest is loaded, configureFsEndpoints() overwrites any roles the
// backend publishes. AXIOS baseURL is '/api/v1/', hence the '/api/v1/fs/…'
// absolute form for direct URL strings.
type FsRole =
  | 'schema' | 'volumes' | 'list' | 'stat'
  | 'download' | 'upload'
  | 'mkdir' | 'delete' | 'rename' | 'writeStatus'
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
  writeStatus: '/api/v1/fs/write/status',
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
  return AXIOS.get(toAxiosPath(FS_PATHS.volumes));
}

export function listDir(path: string): AxiosPromise<FsListResponse> {
  return AXIOS.get(toAxiosPath(FS_PATHS.list), { params: { path } });
}

export function stat(path: string): AxiosPromise<FsEntry> {
  return AXIOS.get(toAxiosPath(FS_PATHS.stat), { params: { path } });
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
  return AXIOS.post(toAxiosPath(FS_PATHS.upload), form, {
    params: { path: targetDir },
    headers,
    onUploadProgress: onUploadProgress as any,
    cancelToken,
  });
}

// mkdir / delete / rename: 200 when done at once; 202 {status:'queued', op}
// when the device's flash writer does it later (the flash write gate is
// busy). settleWrite() waits for a queued one to finish.
export interface FsWriteReply {
  ok: boolean;
  status?: 'queued';
  op?: number;
}

export function mkdir(path: string): AxiosPromise<FsWriteReply> {
  return AXIOS.post(toAxiosPath(FS_PATHS.mkdir), { path });
}

export function remove(path: string, recursive = false): AxiosPromise<FsWriteReply> {
  return AXIOS.post(toAxiosPath(FS_PATHS.delete), { path, recursive });
}

export function rename(from: string, to: string): AxiosPromise<FsWriteReply> {
  return AXIOS.post(toAxiosPath(FS_PATHS.rename), { from, to });
}

export interface FsWriteStatus {
  op: number;
  kind: 'mkdir' | 'delete' | 'rename';
  state: 'queued' | 'running' | 'done' | 'error';
  steps: number;
  path: string;
  error?: string;
}

export function writeStatus(op: number, signal?: AbortSignal): AxiosPromise<FsWriteStatus> {
  return AXIOS.get(toAxiosPath(FS_PATHS.writeStatus), { params: { op }, signal });
}

export const WRITE_POLL_MS = 500;
export const WRITE_SLOW_MS = 120000;  // then: "still running" once, and polls less often
const WRITE_SLOW_POLL_MS = 2000;

// settleWrite() rejections that are not the device's error code.
export const WRITE_STATE_UNKNOWN = 'write_state_unknown';  // the device no longer has it (404)
export const WRITE_ABANDONED = 'write_abandoned';          // the page stopped waiting (signal)

export interface SettleOpts {
  onQueued?: () => void;
  // Once, after WRITE_SLOW_MS: the device is still at it (the steps done so far).
  onSlow?: (st: FsWriteStatus) => void;
  // The page went away: stop polling (rejects with WRITE_ABANDONED).
  signal?: AbortSignal;
}

function pause(ms: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve) => {
    if (signal?.aborted) {
      resolve();
      return;
    }
    const t = setTimeout(done, ms);
    function done() {
      clearTimeout(t);
      signal?.removeEventListener('abort', done);
      resolve();
    }
    signal?.addEventListener('abort', done);
  });
}

// Resolves when the write is done: at once for a 200, after polling the
// write status for a queued one (onQueued is called first). The device
// keeps a queued write until it is done, however long that takes: past
// WRITE_SLOW_MS onSlow says so once and the polls go on, less often.
// Rejects with the device's error code; WRITE_STATE_UNKNOWN when the
// device no longer knows the write (404: dropped as old, or a restart):
// reload the listing to see what is there; WRITE_ABANDONED when signal
// aborts. A 503 (the device was busy) is asked again.
export async function settleWrite(
  reply: AxiosPromise<FsWriteReply>,
  opts: SettleOpts | (() => void) = {}
): Promise<void> {
  const o: SettleOpts = typeof opts === 'function' ? { onQueued: opts } : opts;
  const res = await reply;
  const op = res.data?.op;
  if (res.status !== 202 || res.data?.status !== 'queued' || op === undefined) return;
  if (o.onQueued) o.onQueued();
  const t0 = Date.now();
  let slow = false;
  for (;;) {
    // eslint-disable-next-line no-await-in-loop
    await pause(slow ? WRITE_SLOW_POLL_MS : WRITE_POLL_MS, o.signal);
    if (o.signal?.aborted) throw new Error(WRITE_ABANDONED);
    let st: FsWriteStatus;
    try {
      // eslint-disable-next-line no-await-in-loop
      st = (await writeStatus(op, o.signal)).data;
    } catch (e: any) {
      if (o.signal?.aborted) throw new Error(WRITE_ABANDONED);
      if (e?.response?.status === 503) continue;
      if (e?.response?.status === 404) throw new Error(WRITE_STATE_UNKNOWN);
      throw e;
    }
    if (st.state === 'done') return;
    if (st.state === 'error') throw new Error(st.error || 'write_failed');
    if (!slow && Date.now() - t0 > WRITE_SLOW_MS) {
      slow = true;
      if (o.onSlow) o.onSlow(st);
    }
  }
}

export function fetchSchema(): AxiosPromise<any> {
  return AXIOS.get(toAxiosPath(FS_PATHS.schema));
}

export function formatVolume(volume: string): AxiosPromise<{ ok: boolean }> {
  return AXIOS.post(toAxiosPath(FS_PATHS.format), { volume, confirm: true });
}

export interface FormatStatus {
  state: 'idle' | 'counting' | 'running' | 'done' | 'error';
  percent: number;
  total: number;
  done: number;
}

export function formatStatus(volume: string): AxiosPromise<FormatStatus> {
  return AXIOS.get(toAxiosPath(FS_PATHS.formatStatus), { params: { volume } });
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
  return AXIOS.post(toAxiosPath(FS_PATHS.archivePrepare), { path });
}

export function archiveStatus(): AxiosPromise<ArchiveStatus> {
  return AXIOS.get(toAxiosPath(FS_PATHS.archiveStatus));
}

export function archiveDownloadUrl(): string {
  return FS_PATHS.archiveDownload;
}

export function archiveCancel(): AxiosPromise<{ ok: boolean }> {
  return AXIOS.post(toAxiosPath(FS_PATHS.archiveCancel), {});
}

export function unmountVolume(volume: string): AxiosPromise<{ ok: boolean }> {
  return AXIOS.post(toAxiosPath(FS_PATHS.unmount), { volume });
}

export function mountVolume(volume: string): AxiosPromise<{ ok: boolean }> {
  return AXIOS.post(toAxiosPath(FS_PATHS.mount), { volume });
}

export function readText(path: string): AxiosPromise<string> {
  return AXIOS.get(toAxiosPath(FS_PATHS.download), {
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
  return AXIOS.post(toAxiosPath(FS_PATHS.hashStart), { path });
}

export function hashStatus(): AxiosPromise<HashStatus> {
  return AXIOS.get(toAxiosPath(FS_PATHS.hashStatus));
}

export function hashCancel(): AxiosPromise<{ ok: boolean }> {
  return AXIOS.post(toAxiosPath(FS_PATHS.hashCancel), {});
}

export function writeText(path: string, content: string): AxiosPromise<{ ok: boolean }> {
  const slash = path.lastIndexOf('/');
  const dir = slash > 0 ? path.slice(0, slash) : '/';
  const name = slash >= 0 ? path.slice(slash + 1) : path;
  const form = new FormData();
  form.append('file', new Blob([content], { type: 'text/plain' }), name);
  return AXIOS.post(toAxiosPath(FS_PATHS.upload), form, {
    params: { path: dir },
    headers: { 'Content-Type': 'multipart/form-data' },
  });
}
