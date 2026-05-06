import React, { FC, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import axios, { CancelTokenSource } from 'axios';
import { useSnackbar } from 'notistack';
import {
  Box,
  Breadcrumbs,
  Button,
  Checkbox,
  CircularProgress,
  Dialog,
  DialogActions,
  DialogContent,
  DialogContentText,
  DialogTitle,
  IconButton,
  LinearProgress,
  Link,
  Paper,
  Stack,
  Table,
  TableBody,
  TableCell,
  TableContainer,
  TableHead,
  TableRow,
  TextField as MuiTextField,
  Tooltip,
  Typography,
} from '@mui/material';
import FolderIcon from '@mui/icons-material/Folder';
import InsertDriveFileIcon from '@mui/icons-material/InsertDriveFile';
import CreateNewFolderIcon from '@mui/icons-material/CreateNewFolder';
import DriveFolderUploadIcon from '@mui/icons-material/DriveFolderUpload';
import DownloadIcon from '@mui/icons-material/Download';
import DeleteIcon from '@mui/icons-material/Delete';
import DriveFileRenameOutlineIcon from '@mui/icons-material/DriveFileRenameOutline';
import EditIcon from '@mui/icons-material/Edit';
import AutoFixHighIcon from '@mui/icons-material/AutoFixHigh';
import DeleteForeverIcon from '@mui/icons-material/DeleteForever';
import EjectIcon from '@mui/icons-material/Eject';
import UsbIcon from '@mui/icons-material/Usb';
import ArchiveIcon from '@mui/icons-material/Archive';
import VerifiedIcon from '@mui/icons-material/Verified';
import CloseIcon from '@mui/icons-material/Close';
import ContentCopyIcon from '@mui/icons-material/ContentCopy';

import { Field } from '../types';
import { useFieldParser } from '../utils/useFieldParser';
import {
  FsEntry,
  FsVolume,
  listDir,
  listVolumes,
  mkdir as apiMkdir,
  remove as apiRemove,
  rename as apiRename,
  uploadFile,
  downloadUrl,
  readText,
  writeText,
  formatVolume,
  formatStatus,
  FormatStatus,
  unmountVolume,
  mountVolume,
  archivePrepare,
  archiveStatus,
  archiveDownloadUrl,
  archiveCancel,
  ArchiveStatus,
  hashStart,
  hashStatus,
  hashCancel,
  HashStatus,
} from '../../../api/fs';
import { sha256Hex, crc32Hex } from '../../../utils/hash';

interface FilesFieldProps {
  field: Field;
}

const TEXT_EXTS = new Set([
  'txt', 'json', 'cfg', 'conf', 'ini', 'yml', 'yaml', 'md', 'log',
  'xml', 'csv', 'js', 'ts', 'css', 'html', 'htm', 'env', 'properties',
]);
const MAX_EDIT_BYTES = 256 * 1024;

function extOf(name: string): string {
  const i = name.lastIndexOf('.');
  return i < 0 ? '' : name.slice(i + 1).toLowerCase();
}

function isTextFile(e: FsEntry): boolean {
  if (e.isDir) return false;
  if (e.size > MAX_EDIT_BYTES) return false;
  const ext = extOf(e.name);
  return ext === '' || TEXT_EXTS.has(ext);
}

function joinPath(dir: string, name: string): string {
  if (!dir.endsWith('/')) dir += '/';
  return dir + name;
}

function parentOf(path: string): string {
  if (!path || path === '/') return '/';
  const trimmed = path.endsWith('/') ? path.slice(0, -1) : path;
  const idx = trimmed.lastIndexOf('/');
  if (idx <= 0) return '/';
  return trimmed.slice(0, idx);
}

type UploadItem = { file: File; relPath: string };

function humanSize(bytes: number): string {
  if (!bytes) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB'];
  let v = bytes;
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return `${v.toFixed(v >= 10 || i === 0 ? 0 : 1)} ${units[i]}`;
}

function fmtSpeed(bps: number | null): string {
  if (bps == null || !isFinite(bps) || bps <= 0) return '';
  if (bps >= 1024 * 1024) return `${(bps / (1024 * 1024)).toFixed(2)} MB/s`;
  if (bps >= 1024) return `${(bps / 1024).toFixed(1)} KB/s`;
  return `${Math.round(bps)} B/s`;
}

function fmtTime(seconds: number | null): string {
  if (seconds == null || !isFinite(seconds) || seconds < 0) return '';
  const s = Math.round(seconds);
  if (s < 3600) {
    const m = Math.floor(s / 60);
    const ss = s % 60;
    return `${m}:${ss.toString().padStart(2, '0')}`;
  }
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const ss = s % 60;
  return `${h}:${m.toString().padStart(2, '0')}:${ss.toString().padStart(2, '0')}`;
}

const FilesField: FC<FilesFieldProps> = ({ field }) => {
  const { readableLabel, optionMap } = useFieldParser(field.label, field.o || '');
  const initial = (optionMap as any).initial?.value as string | undefined;
  const { enqueueSnackbar } = useSnackbar();

  const [volumes, setVolumes] = useState<FsVolume[]>([]);
  const [currentPath, setCurrentPath] = useState<string>(initial || '/flash');
  const [entries, setEntries] = useState<FsEntry[]>([]);
  const [readOnly, setReadOnly] = useState<boolean>(false);
  const [loading, setLoading] = useState<boolean>(false);

  const [mkdirOpen, setMkdirOpen] = useState(false);
  const [mkdirName, setMkdirName] = useState('');
  const [renameOpen, setRenameOpen] = useState(false);
  const [renameTarget, setRenameTarget] = useState<FsEntry | null>(null);
  const [renameName, setRenameName] = useState('');
  const [deleteOpen, setDeleteOpen] = useState(false);
  const [deleteTarget, setDeleteTarget] = useState<FsEntry | null>(null);

  // editor
  const [editOpen, setEditOpen] = useState(false);
  const [editTarget, setEditTarget] = useState<FsEntry | null>(null);
  const [editContent, setEditContent] = useState('');
  const [editOriginal, setEditOriginal] = useState('');
  const [editLoading, setEditLoading] = useState(false);
  const [editSaving, setEditSaving] = useState(false);
  const [editError, setEditError] = useState<string | null>(null);

  // format
  const [formatOpen, setFormatOpen] = useState(false);
  const [formatTarget, setFormatTarget] = useState<FsVolume | null>(null);
  const [formatConfirmText, setFormatConfirmText] = useState('');
  const [formatting, setFormatting] = useState(false);
  const [formatProgress, setFormatProgress] = useState<FormatStatus | null>(null);

  // drag-drop
  const [dragOverRow, setDragOverRow] = useState<string | null>(null);
  const [dragOverContainer, setDragOverContainer] = useState(false);
  const [uploading, setUploading] = useState(false);
  const [progress, setProgress] = useState<number | null>(null);
  const [uploadStage, setUploadStage] = useState<'hashing' | 'uploading' | null>(null);
  const [uploadCount, setUploadCount] = useState<{ done: number; total: number } | null>(null);
  const [uploadSpeed, setUploadSpeed] = useState<number | null>(null);
  const [uploadElapsed, setUploadElapsed] = useState<number | null>(null);
  const [uploadEta, setUploadEta] = useState<number | null>(null);
  const [uploadCurrent, setUploadCurrent] = useState<{ relPath: string; targetDir: string } | null>(null);
  const uploadSpeedSamplesRef = useRef<{ t: number; bytes: number }[]>([]);
  const lastSpeedUpdateRef = useRef(0);
  const uploadStartedAtRef = useRef<number>(0);
  const uploadCancelRef = useRef<CancelTokenSource | null>(null);
  const uploadAbortedRef = useRef(false);

  // multi-select
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [lastClickedIdx, setLastClickedIdx] = useState<number | null>(null);
  const tableContainerRef = useRef<HTMLDivElement | null>(null);

  // archive (prepare → poll → download)
  const [archiveOpen, setArchiveOpen] = useState(false);
  const [archiveProgress, setArchiveProgress] = useState<ArchiveStatus | null>(null);
  const [archiveTargetName, setArchiveTargetName] = useState<string>('');
  const [bulkDeleteOpen, setBulkDeleteOpen] = useState(false);

  // verify (per-file hash on device)
  const [verifyOpen, setVerifyOpen] = useState(false);
  const [verifyTarget, setVerifyTarget] = useState<FsEntry | null>(null);
  const [verifyStatus, setVerifyStatus] = useState<HashStatus | null>(null);
  const [verifyCompare, setVerifyCompare] = useState<{ sha: string; match: boolean | null } | null>(null);
  const verifyPollRef = useRef<number | null>(null);

  // Browser opens dropped files by default if dragover/drop are not prevented.
  // Guard the whole window while this component is mounted.
  useEffect(() => {
    const isFileDrag = (e: DragEvent) =>
      !!e.dataTransfer && Array.from(e.dataTransfer.types).includes('Files');
    const stop = (e: DragEvent) => {
      if (isFileDrag(e)) e.preventDefault();
    };
    window.addEventListener('dragover', stop);
    window.addEventListener('drop', stop);
    return () => {
      window.removeEventListener('dragover', stop);
      window.removeEventListener('drop', stop);
    };
  }, []);

  const reloadVolumes = useCallback(async () => {
    try {
      const r = await listVolumes();
      setVolumes(r.data || []);
    } catch (e: any) {
      enqueueSnackbar(e?.message || 'volumes_failed', { variant: 'error' });
    }
  }, [enqueueSnackbar]);

  const reloadDir = useCallback(async (path: string) => {
    if (path === '/' || path === '') {
      setEntries([]);
      setReadOnly(true);
      return;
    }
    setLoading(true);

    try {
      const r = await listDir(path);
      setEntries(r.data.entries || []);
      setReadOnly(!!r.data.readOnly);
    } catch (e: any) {
      enqueueSnackbar(e?.response?.data?.error || e?.message || 'list_failed', { variant: 'error' });
      setEntries([]);
    } finally {
      setLoading(false);
    }
  }, [enqueueSnackbar]);

  useEffect(() => { reloadVolumes(); }, [reloadVolumes]);
  useEffect(() => {
    if (currentPath === '/') reloadVolumes();
    reloadDir(currentPath);
  }, [currentPath, reloadDir, reloadVolumes]);

  const atRoot = currentPath === '/' || currentPath === '';
  const showSelectCol = !atRoot && selected.size > 0;
  const colCount = showSelectCol ? 5 : 4;

  const breadcrumbs = useMemo(() => {
    const parts = currentPath.split('/').filter(Boolean);
    const crumbs: { label: string; path: string }[] = [{ label: '/', path: '/' }];
    let acc = '';
    parts.forEach((p) => {
      acc += '/' + p;
      crumbs.push({ label: p, path: acc });
    });
    return crumbs;
  }, [currentPath]);

  // Multi-file: prefer items API (richer, less truncation across browsers),
  // fall back to files FileList. Both must be inspected synchronously
  // inside the drop handler — DataTransfer becomes empty after the event.
  // Synchronously snapshot DataTransfer items into FileSystemEntry handles
  // (or fallback flat files). MUST run inside the drop handler — items expire
  // once the event finishes. Async traversal is done later by `entriesToItems`.
  const extractDropEntries = (dt: DataTransfer): any[] => {
    const out: any[] = [];
    if (dt.items && dt.items.length) {
      for (let i = 0; i < dt.items.length; i++) {
        const it: any = dt.items[i];
        if (it.kind !== 'file') continue;
        const entry = typeof it.webkitGetAsEntry === 'function' ? it.webkitGetAsEntry() : null;
        if (entry) { out.push(entry); continue; }
        const f = it.getAsFile?.();
        if (f) out.push({ __flatFile: f });
      }
      if (out.length) return out;
    }
    if (dt.files) {
      for (let i = 0; i < dt.files.length; i++) out.push({ __flatFile: dt.files[i] });
    }
    return out;
  };

  // Recursively walks a FileSystemDirectoryEntry, yielding {file, relPath}.
  const readDirectoryEntry = (dirEntry: any, prefix: string): Promise<UploadItem[]> => {
    return new Promise((resolve) => {
      const reader = dirEntry.createReader();
      const all: UploadItem[] = [];
      const readBatch = () => {
        reader.readEntries(async (batch: any[]) => {
          if (!batch.length) { resolve(all); return; }
          for (const child of batch) {
            const sub = await entryToItems(child, prefix);
            all.push(...sub);
          }
          readBatch(); // readEntries returns ~100 at a time
        }, () => resolve(all));
      };
      readBatch();
    });
  };

  const entryToItems = (entry: any, prefix: string): Promise<UploadItem[]> => {
    if (entry.__flatFile) {
      const f: File = entry.__flatFile;
      return Promise.resolve([{ file: f, relPath: prefix + f.name }]);
    }
    if (entry.isFile) {
      return new Promise((resolve) => {
        entry.file(
          (f: File) => resolve([{ file: f, relPath: prefix + f.name }]),
          () => resolve([])
        );
      });
    }
    if (entry.isDirectory) {
      return readDirectoryEntry(entry, prefix + entry.name + '/');
    }
    return Promise.resolve([]);
  };

  const expandEntries = async (rootEntries: any[]): Promise<UploadItem[]> => {
    const all: UploadItem[] = [];
    for (const e of rootEntries) {
      const sub = await entryToItems(e, '');
      all.push(...sub);
    }
    return all;
  };

  const bulkUpload = useCallback(async (targetDir: string, items: UploadItem[]) => {
    if (!items.length) return;

    // Pre-create unique parent directories, parents before children
    const dirSet = new Set<string>();
    for (const { relPath } of items) {
      const parts = relPath.split('/');
      let acc = targetDir;
      for (let i = 0; i < parts.length - 1; i++) {
        acc = joinPath(acc, parts[i]);
        dirSet.add(acc);
      }
    }
    const dirs = Array.from(dirSet).sort((a, b) => a.length - b.length);
    for (const d of dirs) {
      try { await apiMkdir(d); } catch { /* exists / non-fatal */ }
    }

    setUploading(true);
    setProgress(0);
    setUploadCount({ done: 0, total: items.length });
    uploadAbortedRef.current = false;
    let okCount = 0;
    let corruptCount = 0;

    for (let i = 0; i < items.length; i++) {
      if (uploadAbortedRef.current) break;
      const { file, relPath } = items[i];
      const slash = relPath.lastIndexOf('/');
      const subDir = slash > 0 ? relPath.substring(0, slash) : '';
      const tgt = subDir ? joinPath(targetDir, subDir) : targetDir;

      // Hash phase — show indeterminate progress, label "Hashing N/M"
      setUploadStage('hashing');
      setUploadCount({ done: i, total: items.length });
      setUploadCurrent({ relPath, targetDir: tgt });
      setProgress(null);
      let sha: string | undefined;
      let crc: string | undefined;
      try {
        sha = await sha256Hex(file);
        crc = await crc32Hex(file);
      } catch (e: any) {
        enqueueSnackbar(`Hash '${relPath}' failed: ${e?.message || 'hash_failed'}`, { variant: 'warning' });
      }
      if (uploadAbortedRef.current) break;

      // Upload phase — track determinate progress + transfer speed (1.5 s window)
      setUploadStage('uploading');
      setProgress(0);
      uploadSpeedSamplesRef.current = [];
      lastSpeedUpdateRef.current = 0;
      uploadStartedAtRef.current = Date.now();
      setUploadSpeed(null);
      setUploadElapsed(0);
      setUploadEta(null);
      const source = axios.CancelToken.source();
      uploadCancelRef.current = source;
      try {
        await uploadFile(
          tgt,
          file,
          (ev) => {
            if (ev.lengthComputable) {
              const fp = (ev.loaded * 100) / ev.total;
              setProgress(Math.round(((i * 100) + fp) / items.length));
              const now = Date.now();
              const samples = uploadSpeedSamplesRef.current;
              samples.push({ t: now, bytes: ev.loaded });
              while (samples.length > 1 && now - samples[0].t > 1500) samples.shift();
              if (samples.length >= 2 && now - lastSpeedUpdateRef.current > 250) {
                const dt = (samples[samples.length - 1].t - samples[0].t) / 1000;
                const db = samples[samples.length - 1].bytes - samples[0].bytes;
                if (dt > 0.05 && db >= 0) {
                  const sp = db / dt;
                  setUploadSpeed(sp);
                  setUploadElapsed((now - uploadStartedAtRef.current) / 1000);
                  const remain = ev.total - ev.loaded;
                  setUploadEta(sp > 0 ? remain / sp : null);
                  lastSpeedUpdateRef.current = now;
                }
              }
            }
          },
          source.token,
          { sha256: sha, crc32: crc }
        );
        okCount++;
      } catch (e: any) {
        if (axios.isCancel(e)) {
          uploadAbortedRef.current = true;
          break;
        }
        const errCode = e?.response?.data?.error;
        const savedAs = e?.response?.data?.savedAs;
        if (errCode === 'hash_mismatch') {
          corruptCount++;
          enqueueSnackbar(
            `Corrupted upload of '${relPath}'${savedAs ? ` (saved as ${savedAs})` : ''}`,
            { variant: 'error' }
          );
        } else {
          enqueueSnackbar(
            `Failed '${relPath}': ${errCode || e?.message || 'upload_failed'}`,
            { variant: 'error' }
          );
        }
      } finally {
        uploadCancelRef.current = null;
      }
    }

    setUploadStage(null);
    setUploadCount(null);
    await reloadDir(currentPath);
    await reloadVolumes();
    if (uploadAbortedRef.current) {
      enqueueSnackbar(`Upload canceled (${okCount}/${items.length} done)`, { variant: 'warning' });
    } else if (corruptCount > 0) {
      enqueueSnackbar(
        `Uploaded ${okCount}/${items.length} (${corruptCount} corrupted) to ${targetDir}`,
        { variant: 'warning' }
      );
    } else {
      enqueueSnackbar(
        `Uploaded ${okCount}/${items.length} item${items.length > 1 ? 's' : ''} to ${targetDir}`,
        { variant: okCount === items.length ? 'success' : 'warning' }
      );
    }
    setUploading(false);
    setProgress(null);
    setUploadSpeed(null);
    setUploadElapsed(null);
    setUploadEta(null);
    setUploadCurrent(null);
    uploadSpeedSamplesRef.current = [];
    uploadStartedAtRef.current = 0;
    uploadAbortedRef.current = false;
  }, [currentPath, reloadDir, reloadVolumes, enqueueSnackbar]);

  const cancelUpload = useCallback(() => {
    uploadAbortedRef.current = true;
    if (uploadCancelRef.current) {
      uploadCancelRef.current.cancel('user_cancelled');
      uploadCancelRef.current = null;
    }
  }, []);

  const handleDrop = (targetDir: string, dt: DataTransfer) => {
    const dropped = extractDropEntries(dt);
    if (!dropped.length) return;
    expandEntries(dropped).then((items) => bulkUpload(targetDir, items));
  };

  // One uniform drop-target factory. Pass null to disable.
  // Highlight is per-row only (no container outline).
  const dropTarget = (targetDir: string | null) => {
    if (!targetDir) return {};
    const isFileDrag = (e: React.DragEvent) =>
      Array.from(e.dataTransfer.types).includes('Files');
    return {
      onDragEnter: (e: React.DragEvent) => {
        if (!isFileDrag(e)) return;
        e.preventDefault();
        e.stopPropagation();
        setDragOverRow(targetDir);
      },
      onDragOver: (e: React.DragEvent) => {
        if (!isFileDrag(e)) return;
        e.preventDefault();
        e.stopPropagation();
        e.dataTransfer.dropEffect = 'copy';
      },
      onDragLeave: (e: React.DragEvent) => {
        const next = e.relatedTarget as Node | null;
        if (next && (e.currentTarget as Node).contains(next)) return;
        setDragOverRow((prev) => (prev === targetDir ? null : prev));
      },
      onDrop: (e: React.DragEvent) => {
        e.preventDefault();
        e.stopPropagation();
        setDragOverRow(null);
        setDragOverContainer(false);
        handleDrop(targetDir, e.dataTransfer);
      },
    };
  };

  // Drop-zone for the entire current folder (TableContainer).
  // Tracks a separate container-level highlight; specific folder rows still
  // win via dragOverRow so the container outline is suppressed when over a folder.
  const containerDropTarget = (targetDir: string | null) => {
    if (!targetDir) return {};
    const isFileDrag = (e: React.DragEvent) =>
      Array.from(e.dataTransfer.types).includes('Files');
    return {
      onDragEnter: (e: React.DragEvent) => {
        if (!isFileDrag(e)) return;
        e.preventDefault();
        setDragOverContainer(true);
      },
      onDragOver: (e: React.DragEvent) => {
        if (!isFileDrag(e)) return;
        e.preventDefault();
        e.dataTransfer.dropEffect = 'copy';
      },
      onDragLeave: (e: React.DragEvent) => {
        const next = e.relatedTarget as Node | null;
        if (next && (e.currentTarget as Node).contains(next)) return;
        setDragOverContainer(false);
      },
      onDrop: (e: React.DragEvent) => {
        e.preventDefault();
        setDragOverContainer(false);
        // If a folder row caught the drop already, dragOverRow handler ran first.
        if (dragOverRow) return;
        handleDrop(targetDir, e.dataTransfer);
      },
    };
  };

  // Clear selection whenever directory or entry list changes.
  useEffect(() => {
    setSelected(new Set());
    setLastClickedIdx(null);
  }, [currentPath, entries]);

  // Esc clears selection; clicking outside the table container too.
  useEffect(() => {
    if (selected.size === 0) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setSelected(new Set());
    };
    const onMouseDown = (e: MouseEvent) => {
      const node = tableContainerRef.current;
      if (node && !node.contains(e.target as Node)) {
        setSelected(new Set());
      }
    };
    window.addEventListener('keydown', onKey);
    window.addEventListener('mousedown', onMouseDown);
    return () => {
      window.removeEventListener('keydown', onKey);
      window.removeEventListener('mousedown', onMouseDown);
    };
  }, [selected.size]);

  const toggleOne = (name: string, idx: number) => {
    setSelected((prev) => {
      const next = new Set(prev);
      if (next.has(name)) next.delete(name); else next.add(name);
      return next;
    });
    setLastClickedIdx(idx);
  };

  const selectRange = (idx: number) => {
    if (lastClickedIdx === null) {
      toggleOne(entries[idx].name, idx);
      return;
    }
    const [from, to] = lastClickedIdx <= idx ? [lastClickedIdx, idx] : [idx, lastClickedIdx];
    setSelected((prev) => {
      const next = new Set(prev);
      for (let i = from; i <= to; i++) next.add(entries[i].name);
      return next;
    });
  };

  const onCheckboxClick = (e: React.MouseEvent, name: string, idx: number) => {
    e.stopPropagation();
    if (e.shiftKey) selectRange(idx);
    else toggleOne(name, idx);
  };

  // Suppress the click that immediately follows a long-press trigger (touchend/mouseup
  // fires onClick as well), which would otherwise toggle the same row off again.
  const longPressFiredRef = useRef(false);
  const longPressTimerRef = useRef<number | null>(null);

  const beginLongPress = (ent: FsEntry, idx: number) => {
    longPressFiredRef.current = false;
    if (longPressTimerRef.current !== null) {
      window.clearTimeout(longPressTimerRef.current);
    }
    longPressTimerRef.current = window.setTimeout(() => {
      longPressTimerRef.current = null;
      longPressFiredRef.current = true;
      // Enter selection mode with this row selected.
      setSelected((prev) => {
        const next = new Set(prev);
        next.add(ent.name);
        return next;
      });
      setLastClickedIdx(idx);
    }, 500);
  };

  const cancelLongPress = () => {
    if (longPressTimerRef.current !== null) {
      window.clearTimeout(longPressTimerRef.current);
      longPressTimerRef.current = null;
    }
  };

  const onRowNameClick = (e: React.MouseEvent, ent: FsEntry, idx: number) => {
    if (longPressFiredRef.current) {
      // The click that follows the long-press release should not toggle/open.
      longPressFiredRef.current = false;
      e.stopPropagation();
      return;
    }
    if (e.ctrlKey || e.metaKey) { toggleOne(ent.name, idx); return; }
    if (e.shiftKey) { selectRange(idx); return; }
    if (selected.size > 0) { toggleOne(ent.name, idx); return; }
    setLastClickedIdx(idx);
    if (ent.isDir) setCurrentPath(joinPath(currentPath, ent.name));
    else if (isTextFile(ent)) openEditor(ent);
  };

  // Per-row press handlers — wire onMouseDown/onTouchStart to start the long-press timer.
  const longPressHandlers = (ent: FsEntry, idx: number) => ({
    onMouseDown: () => beginLongPress(ent, idx),
    onMouseUp: cancelLongPress,
    onMouseLeave: cancelLongPress,
    onTouchStart: () => beginLongPress(ent, idx),
    onTouchEnd: cancelLongPress,
    onTouchCancel: cancelLongPress,
  });

  const triggerBrowserDownload = (url: string, suggestedName?: string) => {
    const a = document.createElement('a');
    a.href = url;
    if (suggestedName) a.download = suggestedName;
    a.target = '_blank';
    a.rel = 'noreferrer';
    document.body.appendChild(a);
    a.click();
    a.remove();
  };

  // Polls archive job until ready, then triggers browser download.
  const runArchiveJob = useCallback(async (path: string, displayName: string) => {
    setArchiveTargetName(displayName);
    setArchiveProgress({ state: 'walking', walked: 0, totalBytes: 0, entries: 0, path, archiveName: displayName });
    setArchiveOpen(true);
    try {
      await archivePrepare(path);
    } catch (e: any) {
      enqueueSnackbar(`Archive failed: ${e?.response?.data?.error || e?.message || 'prepare_failed'}`, { variant: 'error' });
      setArchiveOpen(false);
      setArchiveProgress(null);
      return;
    }

    const poll = async () => {
      try {
        const { data } = await archiveStatus();
        setArchiveProgress(data);
        if (data.state === 'ready') {
          triggerBrowserDownload(archiveDownloadUrl(), `${data.archiveName || displayName}.tar`);
          setArchiveOpen(false);
          setArchiveProgress(null);
          return;
        }
        if (data.state === 'error') {
          enqueueSnackbar(`Archive error: ${data.error || 'unknown'}`, { variant: 'error' });
          setArchiveOpen(false);
          setArchiveProgress(null);
          return;
        }
      } catch {
        // transient — keep polling
      }
      setTimeout(poll, 500);
    };
    setTimeout(poll, 400);
  }, [enqueueSnackbar]);

  const cancelArchive = async () => {
    try { await archiveCancel(); } catch { /* ignore */ }
    setArchiveOpen(false);
    setArchiveProgress(null);
  };

  // Bulk download: files → direct downloads; folders → archive (one at a time)
  const downloadSelected = async () => {
    const items = entries.filter((e) => selected.has(e.name));
    if (!items.length) return;

    // Files first — staggered to avoid browser blocking simultaneous downloads
    const files = items.filter((e) => !e.isDir);
    for (let i = 0; i < files.length; i++) {
      const f = files[i];
      triggerBrowserDownload(downloadUrl(joinPath(currentPath, f.name)), f.name);
      if (i < files.length - 1) await new Promise((r) => setTimeout(r, 250));
    }

    // Then folders, one TAR job at a time
    const folders = items.filter((e) => e.isDir);
    for (const folder of folders) {
      // eslint-disable-next-line no-await-in-loop
      await runArchiveJob(joinPath(currentPath, folder.name), folder.name);
    }
    setSelected(new Set());
  };

  const deleteSelectedConfirmed = async () => {
    setBulkDeleteOpen(false);
    const items = entries.filter((e) => selected.has(e.name));
    let okCount = 0;
    for (const it of items) {
      try {
        // eslint-disable-next-line no-await-in-loop
        await apiRemove(joinPath(currentPath, it.name), it.isDir);
        okCount++;
      } catch (e: any) {
        enqueueSnackbar(`Delete '${it.name}' failed: ${e?.response?.data?.error || e?.message || 'delete_failed'}`, { variant: 'error' });
      }
    }
    setSelected(new Set());
    await reloadDir(currentPath);
    enqueueSnackbar(`Deleted ${okCount}/${items.length}`, { variant: okCount === items.length ? 'success' : 'warning' });
  };

  // Folder upload via <input webkitdirectory> — delegates to bulkUpload.
  const onFolderInputChange = async (e: React.ChangeEvent<HTMLInputElement>) => {
    const fileList = e.target.files;
    if (!fileList || !fileList.length) return;
    const items: UploadItem[] = [];
    for (let i = 0; i < fileList.length; i++) {
      const f = fileList[i];
      const rel = (f as any).webkitRelativePath || f.name;
      items.push({ file: f, relPath: rel });
    }
    e.target.value = '';
    await bulkUpload(currentPath, items);
  };

  const doMkdir = async () => {
    if (!mkdirName.trim()) return;
    try {
      await apiMkdir(joinPath(currentPath, mkdirName.trim()));
      setMkdirOpen(false);
      setMkdirName('');
      reloadDir(currentPath);
    } catch (e: any) {
      enqueueSnackbar(e?.response?.data?.error || e?.message || 'mkdir_failed', { variant: 'error' });
    }
  };

  const doRename = async () => {
    if (!renameTarget || !renameName.trim()) return;
    try {
      await apiRename(joinPath(currentPath, renameTarget.name), joinPath(currentPath, renameName.trim()));
      setRenameOpen(false);
      setRenameTarget(null);
      reloadDir(currentPath);
    } catch (e: any) {
      enqueueSnackbar(e?.response?.data?.error || e?.message || 'rename_failed', { variant: 'error' });
    }
  };

  const doDelete = async () => {
    if (!deleteTarget) return;
    try {
      await apiRemove(joinPath(currentPath, deleteTarget.name), deleteTarget.isDir);
      setDeleteOpen(false);
      setDeleteTarget(null);
      reloadDir(currentPath);
    } catch (e: any) {
      enqueueSnackbar(e?.response?.data?.error || e?.message || 'delete_failed', { variant: 'error' });
    }
  };

  const doUnmount = async (vol: FsVolume) => {

    try {
      await unmountVolume(vol.name);
      await reloadVolumes();
      if (currentPath.startsWith(vol.mount)) setCurrentPath('/');
    } catch (e: any) {
      enqueueSnackbar(e?.response?.data?.error || e?.message || 'unmount_failed', { variant: 'error' });
    }
  };

  const doMount = async (vol: FsVolume) => {
    try {
      await mountVolume(vol.name);
    } catch (e: any) {
      enqueueSnackbar(e?.response?.data?.error || e?.message || 'mount_failed', { variant: 'error' });
      return;
    }
    // Server runs the actual SD.begin() retries in a background task; poll
    // /volumes a few times until the new state propagates (≤ ~2 s).
    for (let i = 0; i < 8; i++) {
      // eslint-disable-next-line no-await-in-loop
      await new Promise((r) => setTimeout(r, 300));
      // eslint-disable-next-line no-await-in-loop
      const r = await listVolumes();
      const updated = (r.data || []).find((v) => v.name === vol.name);
      if (updated?.mounted) {
        setVolumes(r.data || []);
        enqueueSnackbar(`Mounted '${vol.name}'`, { variant: 'success' });
        return;
      }
    }
    await reloadVolumes();
    enqueueSnackbar(`Mount of '${vol.name}' did not complete — check the card and retry`, { variant: 'warning' });
  };

  const doFormat = async () => {
    if (!formatTarget || formatConfirmText !== 'FORMAT') return;
    const targetName = formatTarget.name;
    const targetMount = formatTarget.mount;
    setFormatting(true);
    setFormatProgress({ state: 'counting', percent: 0, total: 0, done: 0 });

    try {
      await formatVolume(targetName);
    } catch (e: any) {
      enqueueSnackbar(
        e?.response?.data?.error || e?.message || 'format_failed',
        { variant: 'error' }
      );
      setFormatting(false);
      setFormatProgress(null);
      return;
    }

    // poll progress every 500 ms until state == "done" | "error"
    const poll = async () => {
      try {
        const { data } = await formatStatus(targetName);
        setFormatProgress(data);
        if (data.state === 'done' || data.state === 'error') {
          setFormatting(false);
          setFormatOpen(false);
          setFormatTarget(null);
          setFormatConfirmText('');
          setFormatProgress(null);
          await reloadVolumes();
          if (currentPath.startsWith(targetMount)) {
            reloadDir(currentPath);
          }
          enqueueSnackbar(
            data.state === 'done'
              ? `Format of '${targetName}' finished (${data.done} entries removed)`
              : `Format of '${targetName}' failed`,
            { variant: data.state === 'done' ? 'success' : 'error' }
          );
          return;
        }
      } catch (e) {
        // transient HTTP error during format — keep polling
      }
      setTimeout(poll, 500);
    };
    setTimeout(poll, 500);
  };

  const openEditor = async (e: FsEntry) => {
    setEditTarget(e);
    setEditOpen(true);
    setEditLoading(true);
    setEditError(null);
    setEditContent('');
    setEditOriginal('');
    try {
      const r = await readText(joinPath(currentPath, e.name));
      const text = typeof r.data === 'string' ? r.data : String(r.data ?? '');
      let shown = text;
      if (extOf(e.name) === 'json') {
        try {
          shown = JSON.stringify(JSON.parse(text), null, 2);
        } catch {
          shown = text;
        }
      }
      setEditContent(shown);
      setEditOriginal(shown);
    } catch (err: any) {
      setEditError(err?.response?.data?.error || err?.message || 'read_failed');
    } finally {
      setEditLoading(false);
    }
  };

  const closeEditor = () => {
    setEditOpen(false);
    setEditTarget(null);
    setEditContent('');
    setEditOriginal('');
    setEditError(null);
  };

  const isJsonTarget = (): boolean => {
    if (!editTarget) return false;
    return extOf(editTarget.name) === 'json';
  };

  const prettifyJson = () => {
    try {
      const parsed = JSON.parse(editContent);
      setEditContent(JSON.stringify(parsed, null, 2));
      setEditError(null);
    } catch (err: any) {
      setEditError(`JSON parse: ${err?.message || 'invalid'}`);
    }
  };

  const stopVerifyPolling = () => {
    if (verifyPollRef.current !== null) {
      window.clearTimeout(verifyPollRef.current);
      verifyPollRef.current = null;
    }
  };

  const openVerify = async (ent: FsEntry) => {
    setVerifyTarget(ent);
    setVerifyOpen(true);
    setVerifyStatus(null);
    setVerifyCompare(null);
    try {
      await hashStart(joinPath(currentPath, ent.name));
    } catch (e: any) {
      enqueueSnackbar(e?.response?.data?.error || e?.message || 'hash_start_failed', { variant: 'error' });
      setVerifyOpen(false);
      setVerifyTarget(null);
      return;
    }
    const poll = async () => {
      try {
        const { data } = await hashStatus();
        setVerifyStatus(data);
        if (data.state === 'done' || data.state === 'error') return;
      } catch {
        // transient — keep polling
      }
      verifyPollRef.current = window.setTimeout(poll, 500);
    };
    verifyPollRef.current = window.setTimeout(poll, 400);
  };

  const closeVerify = async () => {
    stopVerifyPolling();
    if (verifyStatus?.state === 'running') {
      try { await hashCancel(); } catch { /* ignore */ }
    }
    setVerifyOpen(false);
    setVerifyTarget(null);
    setVerifyStatus(null);
    setVerifyCompare(null);
  };

  const compareWithLocal = async (file: File) => {
    if (!verifyStatus?.sha256) return;
    setVerifyCompare({ sha: '', match: null });
    try {
      const sha = await sha256Hex(file);
      setVerifyCompare({ sha, match: sha === verifyStatus.sha256 });
    } catch (e: any) {
      enqueueSnackbar(`Local hash failed: ${e?.message || 'failed'}`, { variant: 'error' });
      setVerifyCompare(null);
    }
  };

  const copyToClipboard = async (text: string) => {
    try {
      await navigator.clipboard.writeText(text);
      enqueueSnackbar('Copied', { variant: 'success' });
    } catch {
      enqueueSnackbar('Copy failed', { variant: 'error' });
    }
  };

  // Stop polling on unmount
  useEffect(() => () => stopVerifyPolling(), []);

  const saveEditor = async () => {
    if (!editTarget) return;
    setEditSaving(true);
    setEditError(null);
    try {
      await writeText(joinPath(currentPath, editTarget.name), editContent);
      setEditOriginal(editContent);
      await reloadDir(currentPath);
    } catch (err: any) {
      setEditError(err?.response?.data?.error || err?.message || 'save_failed');
    } finally {
      setEditSaving(false);
    }
  };

  return (
    <Box sx={{ width: '100%', my: 2, position: 'relative' }}>
      <Typography variant="h6" gutterBottom>
        {readableLabel}
      </Typography>

      {volumes.length > 0 && (
        <Stack direction="row" spacing={1} sx={{ mb: 1, flexWrap: 'wrap' }}>
          {volumes.map((v) => (
            <Button
              key={v.name}
              size="small"
              variant={currentPath.startsWith(v.mount) ? 'contained' : 'outlined'}
              disabled={!v.mounted}
              onClick={() => setCurrentPath(v.mount)}
            >
              {v.name} {v.mounted ? `(${humanSize(v.used)}/${humanSize(v.total)})` : '(offline)'}
            </Button>
          ))}
        </Stack>
      )}

      <Stack direction="row" spacing={1} alignItems="center" sx={{ mb: 1 }}>
        <Breadcrumbs>
          {breadcrumbs.map((c, i) => (
            <Link
              key={i}
              component="button"
              underline="hover"
              onClick={() => setCurrentPath(c.path)}
              sx={{ cursor: 'pointer' }}
            >
              {c.label}
            </Link>
          ))}
        </Breadcrumbs>
        <Box sx={{ flexGrow: 1 }} />
        {uploading && (
          <>
            <Tooltip
              title={
                uploadCurrent
                  ? `${uploadCurrent.relPath} → ${uploadCurrent.targetDir}`
                  : ''
              }
              placement="top"
            >
              <Typography
                variant="caption"
                color="text.secondary"
                sx={{
                  maxWidth: 480,
                  overflow: 'hidden',
                  textOverflow: 'ellipsis',
                  whiteSpace: 'nowrap',
                }}
              >
                {(() => {
                  if (uploadStage === 'hashing') {
                    const done = (uploadCount?.done ?? 0) + 1;
                    const total = uploadCount?.total ?? 0;
                    const cur = uploadCurrent ? ` · ${uploadCurrent.relPath}` : '';
                    return `Hashing ${done}/${total}${cur}`;
                  }
                  if (progress === null) return 'Uploading…';
                  const speed = uploadSpeed ? ` · ${fmtSpeed(uploadSpeed)}` : '';
                  const elapsed = uploadElapsed != null ? ` · ${fmtTime(uploadElapsed)}` : '';
                  const eta = uploadElapsed != null && uploadEta != null
                    ? ` / ~${fmtTime(uploadEta)}`
                    : '';
                  const cur = uploadCurrent
                    ? ` · ${uploadCurrent.relPath} → ${uploadCurrent.targetDir}`
                    : '';
                  return `Uploading ${progress}%${speed}${elapsed}${eta}${cur}`;
                })()}
              </Typography>
            </Tooltip>
            <Tooltip title="Cancel upload">
              <IconButton onClick={cancelUpload} size="small" color="error">
                <CloseIcon fontSize="small" />
              </IconButton>
            </Tooltip>
          </>
        )}
        {selected.size > 0 && (
          <>
            <Typography variant="caption" color="text.secondary">
              {selected.size} selected
            </Typography>
            <Tooltip title="Download selected">
              <IconButton onClick={downloadSelected} size="small">
                <DownloadIcon />
              </IconButton>
            </Tooltip>
            <Tooltip title="Delete selected">
              <span>
                <IconButton
                  onClick={() => setBulkDeleteOpen(true)}
                  size="small"
                  color="error"
                  disabled={readOnly}
                >
                  <DeleteIcon />
                </IconButton>
              </span>
            </Tooltip>
          </>
        )}
        <Tooltip title="New folder">
          <span>
            <IconButton
              disabled={readOnly || atRoot}
              onClick={() => { setMkdirName(''); setMkdirOpen(true); }}
              size="small"
            >
              <CreateNewFolderIcon />
            </IconButton>
          </span>
        </Tooltip>
        <Tooltip title="Upload folder">
          <span>
            <IconButton
              component="label"
              disabled={readOnly || atRoot || uploading}
              size="small"
            >
              <DriveFolderUploadIcon />
              <input
                type="file"
                hidden
                multiple
                onChange={onFolderInputChange}
                {...({ webkitdirectory: '', directory: '' } as any)}
              />
            </IconButton>
          </span>
        </Tooltip>
      </Stack>

      {(loading || uploading) && (
        <LinearProgress
          sx={{ mb: 1 }}
          variant={progress !== null ? 'determinate' : 'indeterminate'}
          value={progress ?? 0}
        />
      )}
      <TableContainer
        ref={tableContainerRef}
        component={Paper}
        sx={{
          mb: 2,
          outline: (!atRoot && dragOverContainer && !dragOverRow) ? '2px dashed' : 'none',
          outlineColor: 'primary.main',
          outlineOffset: '-2px',
          transition: 'outline-color .12s',
        }}
        {...(!atRoot && !readOnly ? containerDropTarget(currentPath) : {})}
      >
        <Table size="small">
          <TableHead>
            <TableRow>
              {showSelectCol && (
                <TableCell padding="checkbox">
                  <Checkbox
                    size="small"
                    indeterminate={selected.size > 0 && selected.size < entries.length}
                    checked={selected.size === entries.length && entries.length > 0}
                    onChange={(e) => {
                      if (e.target.checked) setSelected(new Set(entries.map((x) => x.name)));
                      else setSelected(new Set());
                    }}
                  />
                </TableCell>
              )}
              <TableCell />
              <TableCell>Name</TableCell>
              <TableCell align="right">Size</TableCell>
              <TableCell align="right">Actions</TableCell>
            </TableRow>
          </TableHead>
          <TableBody>
            {!atRoot && (
              <TableRow
                hover
                sx={{ cursor: 'pointer' }}
                onClick={() => setCurrentPath(parentOf(currentPath))}
              >
                {showSelectCol && <TableCell padding="checkbox" />}
                <TableCell><FolderIcon color="action" /></TableCell>
                <TableCell>..</TableCell>
                <TableCell align="right" />
                <TableCell align="right" />
              </TableRow>
            )}
            {atRoot && volumes.map((v) => {
              const free = v.mounted ? Math.max(0, v.total - v.used) : 0;
              const canDrop = v.mounted && !v.readOnly;
              const isHover = canDrop && dragOverRow === v.mount;
              return (
                <TableRow
                  key={v.mount}
                  hover
                  sx={{
                    cursor: v.mounted ? 'pointer' : 'not-allowed',
                    opacity: v.mounted ? 1 : 0.5,
                    bgcolor: isHover ? 'action.selected' : undefined,
                    transition: 'background-color .12s',
                  }}
                  onClick={() => v.mounted && setCurrentPath(v.mount)}
                  {...(canDrop ? dropTarget(v.mount) : {})}
                >
                  {showSelectCol && <TableCell padding="checkbox" />}
                  <TableCell><FolderIcon color="primary" /></TableCell>
                  <TableCell>
                    {v.name}
                    <Typography variant="caption" color="text.secondary" sx={{ ml: 1 }}>
                      {v.mount} {v.mounted ? '' : '(offline)'} {v.readOnly ? '(ro)' : ''}
                    </Typography>
                  </TableCell>
                  <TableCell align="right">
                    {v.mounted ? `${humanSize(free)} free / ${humanSize(v.total)}` : '—'}
                  </TableCell>
                  <TableCell align="right">
                    {v.name !== 'flash' && v.mounted && (
                      <Tooltip title="Eject (safe remove)">
                        <IconButton
                          size="small"
                          onClick={(e) => { e.stopPropagation(); doUnmount(v); }}
                        >
                          <EjectIcon fontSize="small" />
                        </IconButton>
                      </Tooltip>
                    )}
                    {v.name !== 'flash' && !v.mounted && (
                      <Tooltip title="Mount">
                        <IconButton
                          size="small"
                          color="primary"
                          onClick={(e) => { e.stopPropagation(); doMount(v); }}
                        >
                          <UsbIcon fontSize="small" />
                        </IconButton>
                      </Tooltip>
                    )}
                    {v.canFormat && v.mounted && (
                      <Tooltip title="Format (erase all data)">
                        <IconButton
                          size="small"
                          color="error"
                          onClick={(e) => {
                            e.stopPropagation();
                            setFormatTarget(v);
                            setFormatConfirmText('');
                            setFormatOpen(true);
                          }}
                        >
                          <DeleteForeverIcon fontSize="small" />
                        </IconButton>
                      </Tooltip>
                    )}
                  </TableCell>
                </TableRow>
              );
            })}
            {atRoot && volumes.length === 0 && (
              <TableRow>
                <TableCell colSpan={colCount} align="center">
                  <Typography variant="body2" color="text.secondary">No volumes available</Typography>
                </TableCell>
              </TableRow>
            )}
            {!atRoot && entries.map((e, idx) => {
              const isFolder = e.isDir;
              const target = isFolder && !readOnly ? joinPath(currentPath, e.name) : null;
              const isHover = target !== null && dragOverRow === target;
              const isSelected = selected.has(e.name);
              return (
                <TableRow
                  key={e.name}
                  hover
                  selected={isSelected}
                  sx={{
                    cursor: isFolder ? 'pointer' : 'default',
                    bgcolor: isHover ? 'action.selected' : undefined,
                    transition: 'background-color .12s',
                  }}
                  {...dropTarget(target)}
                  {...longPressHandlers(e, idx)}
                >
                  {showSelectCol && (
                    <TableCell padding="checkbox">
                      <Checkbox
                        size="small"
                        checked={isSelected}
                        onClick={(ev) => onCheckboxClick(ev, e.name, idx)}
                        onChange={() => { /* handled via onClick to capture shift */ }}
                      />
                    </TableCell>
                  )}
                  <TableCell onClick={(ev) => onRowNameClick(ev, e, idx)}>
                    {isFolder ? <FolderIcon color="primary" /> : <InsertDriveFileIcon color="action" />}
                  </TableCell>
                  <TableCell onClick={(ev) => onRowNameClick(ev, e, idx)}>{e.name}</TableCell>
                  <TableCell align="right">{isFolder ? '—' : humanSize(e.size)}</TableCell>
                  <TableCell align="right">
                    {!isFolder && isTextFile(e) && (
                      <Tooltip title="Edit">
                        <IconButton size="small" onClick={() => openEditor(e)}>
                          <EditIcon fontSize="small" />
                        </IconButton>
                      </Tooltip>
                    )}
                    {!isFolder && (
                      <Tooltip title="Verify (compute hash on device)">
                        <IconButton size="small" onClick={() => openVerify(e)}>
                          <VerifiedIcon fontSize="small" />
                        </IconButton>
                      </Tooltip>
                    )}
                    {!isFolder && (
                      <Tooltip title="Download">
                        <IconButton
                          size="small"
                          href={downloadUrl(joinPath(currentPath, e.name))}
                          target="_blank"
                          rel="noreferrer"
                        >
                          <DownloadIcon fontSize="small" />
                        </IconButton>
                      </Tooltip>
                    )}
                    {isFolder && (
                      <Tooltip title="Download as TAR">
                        <IconButton
                          size="small"
                          onClick={() => runArchiveJob(joinPath(currentPath, e.name), e.name)}
                        >
                          <ArchiveIcon fontSize="small" />
                        </IconButton>
                      </Tooltip>
                    )}
                    <Tooltip title="Rename">
                      <span>
                        <IconButton
                          size="small"
                          disabled={readOnly}
                          onClick={() => { setRenameTarget(e); setRenameName(e.name); setRenameOpen(true); }}
                        >
                          <DriveFileRenameOutlineIcon fontSize="small" />
                        </IconButton>
                      </span>
                    </Tooltip>
                    <Tooltip title="Delete">
                      <span>
                        <IconButton
                          size="small"
                          disabled={readOnly}
                          onClick={() => { setDeleteTarget(e); setDeleteOpen(true); }}
                        >
                          <DeleteIcon fontSize="small" />
                        </IconButton>
                      </span>
                    </Tooltip>
                  </TableCell>
                </TableRow>
              );
            })}
            {!loading && !atRoot && entries.length === 0 && (
              <TableRow>
                <TableCell colSpan={colCount} align="center">
                  <Typography variant="body2" color="text.secondary">
                    {readOnly ? 'Empty (read-only)' : 'Empty — drag a file here to upload'}
                  </Typography>
                </TableCell>
              </TableRow>
            )}
          </TableBody>
        </Table>
      </TableContainer>

      {/* archive progress */}
      <Dialog
        open={archiveOpen}
        onClose={() => { /* must use Cancel button */ }}
        disableEscapeKeyDown
      >
        <DialogTitle>Preparing archive</DialogTitle>
        <DialogContent sx={{ minWidth: 360 }}>
          <DialogContentText sx={{ mb: 2 }}>
            <b>{archiveTargetName}</b>
          </DialogContentText>
          <Typography variant="body2" sx={{ mb: 1 }}>
            {archiveProgress?.state === 'walking'
              ? `Scanning… ${archiveProgress.entries} entries, ${humanSize(archiveProgress.totalBytes)}`
              : archiveProgress?.state === 'ready'
                ? 'Starting download…'
                : archiveProgress?.state === 'streaming'
                  ? 'Streaming…'
                  : archiveProgress?.state === 'error'
                    ? `Error: ${archiveProgress.error || 'unknown'}`
                    : 'Starting…'}
          </Typography>
          <LinearProgress
            variant="indeterminate"
            color={archiveProgress?.state === 'error' ? 'error' : 'primary'}
          />
        </DialogContent>
        <DialogActions>
          <Button onClick={cancelArchive} color="error">Cancel</Button>
        </DialogActions>
      </Dialog>

      {/* bulk delete */}
      <Dialog open={bulkDeleteOpen} onClose={() => setBulkDeleteOpen(false)}>
        <DialogTitle>Delete selected items</DialogTitle>
        <DialogContent>
          <DialogContentText>
            Delete <b>{selected.size}</b> selected item{selected.size === 1 ? '' : 's'}?
            Folders will be removed with all their contents.
          </DialogContentText>
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setBulkDeleteOpen(false)}>Cancel</Button>
          <Button onClick={deleteSelectedConfirmed} color="error" variant="contained">Delete</Button>
        </DialogActions>
      </Dialog>

      {/* mkdir */}
      <Dialog open={mkdirOpen} onClose={() => setMkdirOpen(false)}>
        <DialogTitle>New folder</DialogTitle>
        <DialogContent>
          <MuiTextField
            autoFocus
            fullWidth
            label="Folder name"
            value={mkdirName}
            onChange={(e) => setMkdirName(e.target.value)}
            sx={{ mt: 1, minWidth: 300 }}
          />
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setMkdirOpen(false)}>Cancel</Button>
          <Button onClick={doMkdir} variant="contained" disabled={!mkdirName.trim()}>Create</Button>
        </DialogActions>
      </Dialog>

      {/* rename */}
      <Dialog open={renameOpen} onClose={() => setRenameOpen(false)}>
        <DialogTitle>Rename</DialogTitle>
        <DialogContent>
          <MuiTextField
            autoFocus
            fullWidth
            label="New name"
            value={renameName}
            onChange={(e) => setRenameName(e.target.value)}
            sx={{ mt: 1, minWidth: 300 }}
          />
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setRenameOpen(false)}>Cancel</Button>
          <Button onClick={doRename} variant="contained" disabled={!renameName.trim()}>Rename</Button>
        </DialogActions>
      </Dialog>

      {/* delete */}
      <Dialog open={deleteOpen} onClose={() => setDeleteOpen(false)}>
        <DialogTitle>Delete {deleteTarget?.isDir ? 'folder' : 'file'}</DialogTitle>
        <DialogContent>
          <DialogContentText>
            Delete <b>{deleteTarget?.name}</b>{deleteTarget?.isDir ? ' and all its contents' : ''}?
          </DialogContentText>
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setDeleteOpen(false)}>Cancel</Button>
          <Button onClick={doDelete} color="error" variant="contained">Delete</Button>
        </DialogActions>
      </Dialog>

      {/* editor */}
      <Dialog open={editOpen} onClose={closeEditor} maxWidth="md" fullWidth>
        <DialogTitle>
          {editTarget?.name || 'Edit'}
          {readOnly && <Typography component="span" variant="caption" sx={{ ml: 1 }}>(read-only)</Typography>}
        </DialogTitle>
        <DialogContent dividers>
          {editLoading ? (
            <Box sx={{ display: 'flex', justifyContent: 'center', p: 4 }}>
              <CircularProgress />
            </Box>
          ) : (
            <MuiTextField
              multiline
              fullWidth
              minRows={18}
              maxRows={30}
              value={editContent}
              onChange={(e) => setEditContent(e.target.value)}
              InputProps={{
                readOnly,
                sx: { fontFamily: 'monospace', fontSize: 13, whiteSpace: 'pre' },
              }}
            />
          )}
          {editError && (
            <Typography color="error" variant="body2" sx={{ mt: 1 }}>
              {editError}
            </Typography>
          )}
        </DialogContent>
        <DialogActions>
          {isJsonTarget() && (
            <Button
              onClick={prettifyJson}
              startIcon={<AutoFixHighIcon />}
              disabled={editLoading || !editContent}
            >
              Format
            </Button>
          )}
          <Button onClick={closeEditor}>Close</Button>
          <Button
            onClick={saveEditor}
            variant="contained"
            disabled={readOnly || editSaving || editLoading || editContent === editOriginal}
          >
            {editSaving ? 'Saving…' : 'Save'}
          </Button>
        </DialogActions>
      </Dialog>

      {/* verify (per-file device hash) */}
      <Dialog open={verifyOpen} onClose={closeVerify} maxWidth="sm" fullWidth>
        <DialogTitle>Verify {verifyTarget?.name}</DialogTitle>
        <DialogContent>
          {(!verifyStatus || verifyStatus.state === 'running') && (
            <Box sx={{ my: 2 }}>
              <Typography variant="body2" sx={{ mb: 1 }}>
                {verifyStatus
                  ? `Hashing on device — ${humanSize(verifyStatus.bytesRead)} / ${humanSize(verifyStatus.total)} (${verifyStatus.percent}%)`
                  : 'Starting…'}
              </Typography>
              <LinearProgress
                variant={verifyStatus ? 'determinate' : 'indeterminate'}
                value={verifyStatus?.percent ?? 0}
              />
            </Box>
          )}
          {verifyStatus?.state === 'error' && (
            <Typography color="error" variant="body2" sx={{ my: 2 }}>
              Error: {verifyStatus.error || 'unknown'}
            </Typography>
          )}
          {verifyStatus?.state === 'done' && (
            <Box sx={{ my: 2 }}>
              <Typography variant="caption" color="text.secondary">SHA-256</Typography>
              <Box sx={{ display: 'flex', alignItems: 'center', mb: 1 }}>
                <Typography
                  variant="body2"
                  sx={{ fontFamily: 'monospace', wordBreak: 'break-all', flexGrow: 1 }}
                >
                  {verifyStatus.sha256}
                </Typography>
                <Tooltip title="Copy">
                  <IconButton size="small" onClick={() => copyToClipboard(verifyStatus.sha256 || '')}>
                    <ContentCopyIcon fontSize="small" />
                  </IconButton>
                </Tooltip>
              </Box>
              <Typography variant="caption" color="text.secondary">CRC32</Typography>
              <Box sx={{ display: 'flex', alignItems: 'center', mb: 2 }}>
                <Typography
                  variant="body2"
                  sx={{ fontFamily: 'monospace', flexGrow: 1 }}
                >
                  {verifyStatus.crc32}
                </Typography>
                <Tooltip title="Copy">
                  <IconButton size="small" onClick={() => copyToClipboard(verifyStatus.crc32 || '')}>
                    <ContentCopyIcon fontSize="small" />
                  </IconButton>
                </Tooltip>
              </Box>
              <Box sx={{ display: 'flex', alignItems: 'center', gap: 1 }}>
                <Button component="label" size="small" variant="outlined">
                  Compare with local…
                  <input
                    type="file"
                    hidden
                    onChange={(e) => {
                      const f = e.target.files?.[0];
                      if (f) compareWithLocal(f);
                      e.target.value = '';
                    }}
                  />
                </Button>
                {verifyCompare?.match === true && (
                  <Typography variant="body2" color="success.main">Match</Typography>
                )}
                {verifyCompare?.match === false && (
                  <Typography variant="body2" color="error.main">Mismatch</Typography>
                )}
                {verifyCompare && verifyCompare.match === null && (
                  <Typography variant="body2" color="text.secondary">Hashing…</Typography>
                )}
              </Box>
              {verifyCompare?.sha && verifyCompare.match === false && (
                <Typography
                  variant="caption"
                  sx={{ fontFamily: 'monospace', wordBreak: 'break-all', display: 'block', mt: 1 }}
                  color="text.secondary"
                >
                  local: {verifyCompare.sha}
                </Typography>
              )}
            </Box>
          )}
        </DialogContent>
        <DialogActions>
          <Button onClick={closeVerify}>
            {verifyStatus?.state === 'running' ? 'Cancel' : 'Close'}
          </Button>
        </DialogActions>
      </Dialog>

      {/* format volume */}
      <Dialog
        open={formatOpen}
        onClose={() => { if (!formatting) { setFormatOpen(false); setFormatTarget(null); } }}
      >
        <DialogTitle color="error">
          Format {formatTarget?.name?.toUpperCase()}
        </DialogTitle>
        <DialogContent>
          <DialogContentText sx={{ mb: 2 }}>
            This will <b>permanently erase ALL data</b> on
            volume <b>{formatTarget?.name}</b>.
            This action cannot be undone.
          </DialogContentText>
          <DialogContentText sx={{ mb: 2 }}>
            Type <b>FORMAT</b> to confirm:
          </DialogContentText>
          <MuiTextField
            autoFocus
            fullWidth
            value={formatConfirmText}
            onChange={(e) => setFormatConfirmText(e.target.value.toUpperCase())}
            placeholder="FORMAT"
            disabled={formatting}
            sx={{ minWidth: 300 }}
          />
          {formatting && formatProgress && (
            <Box sx={{ mt: 3 }}>
              <Typography variant="body2" sx={{ mb: 1 }}>
                {formatProgress.state === 'counting'
                  ? 'Scanning files…'
                  : formatProgress.state === 'running'
                    ? `Deleting ${formatProgress.done} / ${formatProgress.total} (${formatProgress.percent}%)`
                    : formatProgress.state === 'done'
                      ? 'Done'
                      : formatProgress.state === 'error'
                        ? 'Error'
                        : 'Starting…'}
              </Typography>
              <LinearProgress
                variant={formatProgress.state === 'counting' ? 'indeterminate' : 'determinate'}
                value={formatProgress.percent}
                color={formatProgress.state === 'error' ? 'error' : 'primary'}
              />
            </Box>
          )}
        </DialogContent>
        <DialogActions>
          <Button
            onClick={() => { setFormatOpen(false); setFormatTarget(null); }}
            disabled={formatting}
          >
            Cancel
          </Button>
          <Button
            onClick={doFormat}
            color="error"
            variant="contained"
            disabled={formatConfirmText !== 'FORMAT' || formatting}
          >
            {formatting ? 'Formatting…' : 'Format'}
          </Button>
        </DialogActions>
      </Dialog>
    </Box>
  );
};

export default FilesField;
