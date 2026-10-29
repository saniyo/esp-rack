#include "FileSystemManager.h"

#include <power/FlashWriter.h>

#include "FsPath.h"
#include "FsPromote.h"

// ---- FsWriteIo ----

FsWriteIo::FsWriteIo() : _mx(xSemaphoreCreateMutex()), _stepMx(xSemaphoreCreateMutex()) {}

bool FsWriteIo::lock(uint32_t ms) { return _mx && xSemaphoreTake(_mx, pdMS_TO_TICKS(ms)) == pdTRUE; }

void FsWriteIo::unlock() { xSemaphoreGive(_mx); }

bool FsWriteIo::stepLock(uint32_t ms) { return _stepMx && xSemaphoreTake(_stepMx, pdMS_TO_TICKS(ms)) == pdTRUE; }

void FsWriteIo::stepUnlock() { xSemaphoreGive(_stepMx); }

void FsWriteIo::kick() { ESPRack::FlashWriter::kick(_job); }

int8_t FsWriteIo::kind(Vol fs, const String& path) {
  File f = fs->open(path);
  if (!f) return -1;
  const bool dir = f.isDirectory();
  f.close();
  return dir ? 1 : 0;
}

// Its full path; getNextFileName opens no File per entry.
bool FsWriteIo::firstChild(Vol fs, const String& dir, String& child, bool& isDir) {
  File d = fs->open(dir);
  if (!d) return false;
  if (!d.isDirectory()) {
    d.close();
    return false;
  }
  bool found = false;
  for (;;) {
    bool isD = false;
    String n = d.getNextFileName(&isD);
    if (!n.length()) break;
    if (n.endsWith("/.") || n.endsWith("/..")) continue;
    child = n;
    isDir = isD;
    found = true;
    break;
  }
  d.close();
  return found;
}

// ---- FileSystemManager ----

FileSystemManager::FileSystemManager() {
  // At setup (the loop task): the FlashWriter job for the writes the gate
  // puts off. Without one they are refused while the gate is not free.
  const int8_t job = ESPRack::FlashWriter::add("fs", [this] { return _writes.run(); });
  if (job < 0) log_e("[fs] no FlashWriter job left: writes the gate puts off are refused");
  _io.setJob(job);
  // The kept writes at a restart: after every module hook, with the config's.
  const bool flush = ESPRack::FlashWriter::addRestartFlush(
      [](void* self, uint32_t boundMs) { static_cast<FileSystemManager*>(self)->shutdown(boundMs); },
      [](void* self, uint32_t boundMs) -> uint32_t {
        return static_cast<FileSystemManager*>(self)->writesPending() ? boundMs : 0;
      },
      this);
  if (!flush) log_e("[fs] no restart flush left: writes kept at a restart go with it");
}

void FileSystemManager::addBackend(std::unique_ptr<FileSystemBackend> backend) {
  if (!backend) return;
  _backends.emplace_back(std::move(backend));
}

bool FileSystemManager::sanitize(const String& absPath, String& cleaned) { return fsNormalise(absPath, cleaned); }

FileSystemBackend* FileSystemManager::resolve(const String& absPath, String& relOut) const {
  String clean;
  if (!sanitize(absPath, clean)) return nullptr;

  // split first segment
  int second = clean.indexOf('/', 1);
  String volume = (second < 0) ? clean.substring(1) : clean.substring(1, second);
  String rel = (second < 0) ? String("/") : clean.substring(second);
  if (rel.length() == 0) rel = "/";

  for (auto& b : _backends) {
    if (volume == b->name()) {
      relOut = rel;
      return b.get();
    }
  }
  return nullptr;
}

bool FileSystemManager::isReadOnlyPath(const String& absPath) const {
  String clean;
  if (!sanitize(absPath, clean)) return true;
  for (auto& p : _readOnlyPrefixes) {
    if (fsWithin(clean, p)) return true;
  }
  return false;
}

bool FileSystemManager::coversReadOnlyPath(const String& absPath) const {
  String clean;
  if (!sanitize(absPath, clean)) return true;
  for (auto& p : _readOnlyPrefixes) {
    if (fsWithin(clean, p) || fsWithin(p, clean)) return true;
  }
  return false;
}

const char* FileSystemManager::resultString(FsResult r) {
  switch (r) {
    case FsResult::OK: return "ok";
    case FsResult::NOT_FOUND: return "not_found";
    case FsResult::NOT_MOUNTED: return "not_mounted";
    case FsResult::READ_ONLY: return "read_only";
    case FsResult::INVALID_PATH: return "invalid_path";
    case FsResult::IO_ERROR: return "io_error";
    case FsResult::EXISTS: return "exists";
    case FsResult::QUEUED: return "queued";
    case FsResult::BUSY: return "busy";
  }
  return "unknown";
}

FsResult FileSystemManager::list(const String& absPath, std::vector<FileSystemEntry>& out) {
  String rel;
  FileSystemBackend* b = resolve(absPath, rel);
  if (!b) return FsResult::INVALID_PATH;
  if (!b->isMounted()) return FsResult::NOT_MOUNTED;
  fs::FS* fs = b->fs();
  if (!fs) return FsResult::NOT_MOUNTED;

  File dir = fs->open(rel);
  if (!dir) return FsResult::NOT_FOUND;
  if (!dir.isDirectory()) { dir.close(); return FsResult::INVALID_PATH; }

  File f = dir.openNextFile();
  while (f) {
    FileSystemEntry e;
    const char* n = f.name();
    // LittleFS returns full path in name(); take last segment for display
    const char* slash = strrchr(n, '/');
    e.name = slash ? String(slash + 1) : String(n);
    e.isDir = f.isDirectory();
    e.size = f.isDirectory() ? 0 : f.size();
    e.mtime = (uint32_t)f.getLastWrite();
    out.emplace_back(std::move(e));
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  return FsResult::OK;
}

FsResult FileSystemManager::stat(const String& absPath, FileSystemEntry& out) {
  String rel;
  FileSystemBackend* b = resolve(absPath, rel);
  if (!b) return FsResult::INVALID_PATH;
  if (!b->isMounted()) return FsResult::NOT_MOUNTED;
  fs::FS* fs = b->fs();
  if (!fs) return FsResult::NOT_MOUNTED;

  File f = fs->open(rel);
  if (!f) return FsResult::NOT_FOUND;
  const char* n = f.name();
  const char* slash = strrchr(n, '/');
  out.name = slash ? String(slash + 1) : String(n);
  out.isDir = f.isDirectory();
  out.size = f.isDirectory() ? 0 : f.size();
  out.mtime = (uint32_t)f.getLastWrite();
  f.close();
  return FsResult::OK;
}

FsResult FileSystemManager::mkdir(const String& absPath) {
  if (isReadOnlyPath(absPath)) return FsResult::READ_ONLY;
  String rel;
  FileSystemBackend* b = resolve(absPath, rel);
  if (!b) return FsResult::INVALID_PATH;
  if (!b->isMounted()) return FsResult::NOT_MOUNTED;
  if (b->readOnly()) return FsResult::READ_ONLY;
  fs::FS* fs = b->fs();
  if (!fs) return FsResult::NOT_MOUNTED;

  if (fs->exists(rel)) return FsResult::EXISTS;
  return fs->mkdir(rel) ? FsResult::OK : FsResult::IO_ERROR;
}

FsResult FileSystemManager::remove(const String& absPath, bool recursive) {
  if (recursive ? coversReadOnlyPath(absPath) : isReadOnlyPath(absPath)) return FsResult::READ_ONLY;
  String rel;
  FileSystemBackend* b = resolve(absPath, rel);
  if (!b) return FsResult::INVALID_PATH;
  if (!b->isMounted()) return FsResult::NOT_MOUNTED;
  if (b->readOnly()) return FsResult::READ_ONLY;
  fs::FS* fs = b->fs();
  if (!fs) return FsResult::NOT_MOUNTED;

  File f = fs->open(rel);
  if (!f) return FsResult::NOT_FOUND;
  bool isDir = f.isDirectory();
  f.close();

  if (isDir) {
    if (recursive) return WriteQueue::removeTree(_io, fs, rel);
    return fs->rmdir(rel) ? FsResult::OK : FsResult::IO_ERROR;
  }
  return fs->remove(rel) ? FsResult::OK : FsResult::IO_ERROR;
}

namespace {

// FsPromote on a mounted volume (upload debris at boot).
struct VolumeIo {
  using Str = String;
  fs::FS& fs;
  bool replaces() const { return false; }  // restoreOld() does not ask
  bool exists(const String& p) { return fs.exists(p); }
  bool remove(const String& p) { return fs.remove(p); }
  FsResult rename(const String& from, const String& to) {
    return fs.rename(from, to) ? FsResult::OK : FsResult::IO_ERROR;
  }
};

}  // namespace

void FileSystemManager::purgeUploadDebris(fs::FS& fs, const char* root) {
  // Iterative BFS so the call frame stays constant — safe to run from a
  // small (e.g. SD mount-watcher) FreeRTOS task without blowing the stack.
  std::vector<String> queue;
  queue.emplace_back(root);
  while (!queue.empty()) {
    String path = queue.back();
    queue.pop_back();
    File dir = fs.open(path);
    if (!dir) continue;
    if (!dir.isDirectory()) { dir.close(); continue; }

    std::vector<String> victims;
    std::vector<String> olds;  // a promotion cut short (FsPromote.h)
    File child = dir.openNextFile();
    while (child) {
      const char* n = child.name();
      String full = (n && n[0] == '/') ? String(n)
                                       : (path.endsWith("/") ? (path + n) : (path + "/" + n));
      if (child.isDirectory()) {
        queue.emplace_back(full);
      } else if (full.endsWith(".partial")) {
        // ONLY upload debris (LIB-FIX-007). Uploads write <target>.partial
        // and promote on success, so a crash leaves nothing else behind;
        // zero-byte files are legitimate (markers, empty logs, configs).
        victims.emplace_back(full);
      } else if (full.endsWith(kOldSuffix)) {
        olds.emplace_back(full);
      }
      child.close();
      child = dir.openNextFile();
    }
    dir.close();

    for (auto& v : victims) {
      if (fs.remove(v)) {
        log_d("[fs] purged debris: %s", v.c_str());
      }
    }
    VolumeIo io{fs};
    for (auto& o : olds) {
      if (FsPromote<VolumeIo>::restoreOld(io, o)) log_w("[fs] upload cut short: settled %s", o.c_str());
    }
  }
}

FsResult FileSystemManager::rename(const String& fromAbs, const String& toAbs) {
  if (coversReadOnlyPath(fromAbs) || isReadOnlyPath(toAbs)) return FsResult::READ_ONLY;
  String fromRel, toRel;
  FileSystemBackend* bf = resolve(fromAbs, fromRel);
  FileSystemBackend* bt = resolve(toAbs, toRel);
  if (!bf || !bt) return FsResult::INVALID_PATH;
  if (bf != bt) return FsResult::INVALID_PATH;  // cross-volume rename not supported
  if (!bf->isMounted()) return FsResult::NOT_MOUNTED;
  if (bf->readOnly()) return FsResult::READ_ONLY;
  fs::FS* fs = bf->fs();
  if (!fs) return FsResult::NOT_MOUNTED;

  if (!fs->exists(fromRel)) return FsResult::NOT_FOUND;
  if (fs->exists(toRel)) return FsResult::EXISTS;
  return fs->rename(fromRel, toRel) ? FsResult::OK : FsResult::IO_ERROR;
}

bool FileSystemManager::exists(const String& absPath) {
  String rel;
  FileSystemBackend* b = resolve(absPath, rel);
  if (!b || !b->isMounted()) return false;
  fs::FS* fs = b->fs();
  return fs && fs->exists(rel);
}

bool FileSystemManager::renameReplaces(const String& absPath) const {
  String rel;
  const FileSystemBackend* b = resolve(absPath, rel);
  return b && b->renameReplaces();
}

FsResult FileSystemManager::renameOver(const String& fromAbs, const String& toAbs) {
  if (isReadOnlyPath(fromAbs) || isReadOnlyPath(toAbs)) return FsResult::READ_ONLY;
  String fromRel, toRel;
  FileSystemBackend* bf = resolve(fromAbs, fromRel);
  FileSystemBackend* bt = resolve(toAbs, toRel);
  if (!bf || !bt) return FsResult::INVALID_PATH;
  if (bf != bt) return FsResult::INVALID_PATH;
  if (!bf->isMounted()) return FsResult::NOT_MOUNTED;
  if (bf->readOnly()) return FsResult::READ_ONLY;
  fs::FS* fs = bf->fs();
  if (!fs) return FsResult::NOT_MOUNTED;
  if (!fs->exists(fromRel)) return FsResult::NOT_FOUND;
  if (!bf->renameReplaces() && fs->exists(toRel)) return FsResult::EXISTS;
  return fs->rename(fromRel, toRel) ? FsResult::OK : FsResult::IO_ERROR;
}

File FileSystemManager::openRead(const String& absPath) {
  String rel;
  FileSystemBackend* b = resolve(absPath, rel);
  if (!b || !b->isMounted()) return File();
  fs::FS* fs = b->fs();
  if (!fs) return File();
  return fs->open(rel, "r");
}

File FileSystemManager::openWrite(const String& absPath, FsResult& resOut) {
  if (isReadOnlyPath(absPath)) { resOut = FsResult::READ_ONLY; return File(); }
  String rel;
  FileSystemBackend* b = resolve(absPath, rel);
  if (!b) { resOut = FsResult::INVALID_PATH; return File(); }
  if (!b->isMounted()) { resOut = FsResult::NOT_MOUNTED; return File(); }
  if (b->readOnly()) { resOut = FsResult::READ_ONLY; return File(); }
  fs::FS* fs = b->fs();
  if (!fs) { resOut = FsResult::NOT_MOUNTED; return File(); }

  File f = fs->open(rel, "w");
  resOut = f ? FsResult::OK : FsResult::IO_ERROR;
  return f;
}

// ---- writes through the flash write gate ----

FsResult FileSystemManager::writable(const String& absPath, fs::FS*& fs, String& rel,
                                     FileSystemBackend** bOut) const {
  if (isReadOnlyPath(absPath)) return FsResult::READ_ONLY;
  FileSystemBackend* b = resolve(absPath, rel);
  if (!b) return FsResult::INVALID_PATH;
  if (!b->isMounted()) return FsResult::NOT_MOUNTED;
  if (b->readOnly()) return FsResult::READ_ONLY;
  fs = b->fs();
  if (!fs) return FsResult::NOT_MOUNTED;
  if (bOut) *bOut = b;
  return FsResult::OK;
}

FsResult FileSystemManager::mkdirGated(const String& absPath, uint32_t* op) {
  if (op) *op = 0;
  fs::FS* fs = nullptr;
  String rel;
  FileSystemBackend* b = nullptr;
  const FsResult r = writable(absPath, fs, rel, &b);
  if (r != FsResult::OK) return r;
  if (!b->onCodeFlash()) return mkdir(absPath);
  return _writes.submit(WriteQueue::Kind::Mkdir, fs, rel, String(), op);
}

FsResult FileSystemManager::removeGated(const String& absPath, bool recursive, uint32_t* op) {
  if (op) *op = 0;
  fs::FS* fs = nullptr;
  String rel;
  FileSystemBackend* b = nullptr;
  const FsResult r = writable(absPath, fs, rel, &b);
  if (r != FsResult::OK) return r;
  if (recursive && coversReadOnlyPath(absPath)) return FsResult::READ_ONLY;  // a protected tree inside
  if (!b->onCodeFlash()) return remove(absPath, recursive);
  return _writes.submit(recursive ? WriteQueue::Kind::RemoveTree : WriteQueue::Kind::Remove, fs, rel, String(), op);
}

FsResult FileSystemManager::renameGated(const String& fromAbs, const String& toAbs, uint32_t* op) {
  if (op) *op = 0;
  if (coversReadOnlyPath(fromAbs) || isReadOnlyPath(toAbs)) return FsResult::READ_ONLY;
  String fromRel, toRel;
  FileSystemBackend* bf = resolve(fromAbs, fromRel);
  FileSystemBackend* bt = resolve(toAbs, toRel);
  if (!bf || !bt) return FsResult::INVALID_PATH;
  if (bf != bt) return FsResult::INVALID_PATH;  // cross-volume rename not supported
  if (!bf->isMounted()) return FsResult::NOT_MOUNTED;
  if (bf->readOnly()) return FsResult::READ_ONLY;
  fs::FS* fs = bf->fs();
  if (!fs) return FsResult::NOT_MOUNTED;
  if (!bf->onCodeFlash()) return rename(fromAbs, toAbs);
  return _writes.submit(WriteQueue::Kind::Rename, fs, fromRel, toRel, op);
}

void FileSystemManager::shutdown(uint32_t boundMs) {
  _writes.shutdown(boundMs);
  const uint8_t left = _writes.pending();
  if (!left) return;
  log_w("[fs] restart: %u file manager write(s) asked for and not done", (unsigned)left);
  const bool listed = _writes.forEachPending([](const WriteQueue::Status& s) {
    static const char* const kKinds[] = {"mkdir", "delete", "delete tree", "rename"};
    log_w("[fs]   #%u %s %s: %u step(s) done", (unsigned)s.id, kKinds[static_cast<uint8_t>(s.kind)], s.path.c_str(),
          (unsigned)s.steps);
  });
  if (!listed) log_w("[fs]   (not listed: the queue was busy)");
}
