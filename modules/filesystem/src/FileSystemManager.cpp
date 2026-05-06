#include "FileSystemManager.h"

FileSystemManager::FileSystemManager() {}

void FileSystemManager::addBackend(std::unique_ptr<FileSystemBackend> backend) {
  if (!backend) return;
  _backends.emplace_back(std::move(backend));
}

bool FileSystemManager::sanitize(const String& absPath, String& cleaned) {
  if (absPath.length() == 0 || absPath.charAt(0) != '/') return false;
  // reject ".." segments
  if (absPath.indexOf("/..") >= 0) return false;
  cleaned = absPath;
  // collapse duplicated slashes
  while (cleaned.indexOf("//") >= 0) cleaned.replace("//", "/");
  // strip trailing slash (except root)
  if (cleaned.length() > 1 && cleaned.endsWith("/")) cleaned.remove(cleaned.length() - 1);
  return true;
}

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
    if (clean == p || clean.startsWith(p + "/") || clean.startsWith(p)) {
      if (clean == p || clean.startsWith(p + "/")) return true;
    }
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

bool FileSystemManager::removeRecursive(fs::FS& fs, const String& path) {
  File f = fs.open(path);
  if (!f) return false;
  bool isDir = f.isDirectory();
  if (!isDir) {
    f.close();
    return fs.remove(path);
  }
  File child = f.openNextFile();
  while (child) {
    String n = child.name();
    // normalize: ensure absolute
    String childPath = n.startsWith("/") ? n : (path.endsWith("/") ? (path + n) : (path + "/" + n));
    bool childDir = child.isDirectory();
    child.close();
    if (childDir) {
      if (!removeRecursive(fs, childPath)) { f.close(); return false; }
    } else {
      if (!fs.remove(childPath)) { f.close(); return false; }
    }
    child = f.openNextFile();
  }
  f.close();
  return fs.rmdir(path);
}

FsResult FileSystemManager::remove(const String& absPath, bool recursive) {
  if (isReadOnlyPath(absPath)) return FsResult::READ_ONLY;
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
    if (recursive) {
      return removeRecursive(*fs, rel) ? FsResult::OK : FsResult::IO_ERROR;
    }
    return fs->rmdir(rel) ? FsResult::OK : FsResult::IO_ERROR;
  }
  return fs->remove(rel) ? FsResult::OK : FsResult::IO_ERROR;
}

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
    File child = dir.openNextFile();
    while (child) {
      const char* n = child.name();
      String full = (n && n[0] == '/') ? String(n)
                                       : (path.endsWith("/") ? (path + n) : (path + "/" + n));
      if (child.isDirectory()) {
        queue.emplace_back(full);
      } else if (full.endsWith(".partial") || child.size() == 0) {
        victims.emplace_back(full);
      }
      child.close();
      child = dir.openNextFile();
    }
    dir.close();

    for (auto& v : victims) {
      if (fs.remove(v)) {
        Serial.printf("[fs] purged debris: %s\n", v.c_str());
      }
    }
  }
}

FsResult FileSystemManager::rename(const String& fromAbs, const String& toAbs) {
  if (isReadOnlyPath(fromAbs) || isReadOnlyPath(toAbs)) return FsResult::READ_ONLY;
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
