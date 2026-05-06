#ifndef FileSystemManager_h
#define FileSystemManager_h

#include <Arduino.h>
#include <FS.h>
#include <vector>
#include <memory>

#include "FileSystemBackend.h"

struct FileSystemEntry {
  String name;
  bool isDir;
  size_t size;
  uint32_t mtime;  // 0 if unavailable
};

enum class FsResult {
  OK,
  NOT_FOUND,
  NOT_MOUNTED,
  READ_ONLY,
  INVALID_PATH,
  IO_ERROR,
  EXISTS,
};

class FileSystemManager {
 public:
  FileSystemManager();

  // Backends are mounted and added. Flash backend is always present.
  void addBackend(std::unique_ptr<FileSystemBackend> backend);

  // Return the volumes list (for UI).
  const std::vector<std::unique_ptr<FileSystemBackend>>& backends() const { return _backends; }

  // Read-only path prefixes (HTTP API will reject writes under these).
  void addReadOnlyPrefix(const char* prefix) { _readOnlyPrefixes.emplace_back(prefix); }

  // Parse absolute path "/<volume>/<rel>" -> (backend, relative-to-root path starting with "/").
  // Returns nullptr if volume not found.
  FileSystemBackend* resolve(const String& absPath, String& relOut) const;

  bool isReadOnlyPath(const String& absPath) const;

  // ---- operations (absolute paths starting with "/<volume>/...") ----
  FsResult list(const String& absPath, std::vector<FileSystemEntry>& out);
  FsResult stat(const String& absPath, FileSystemEntry& out);
  FsResult mkdir(const String& absPath);
  FsResult remove(const String& absPath, bool recursive);
  FsResult rename(const String& fromAbs, const String& toAbs);

  // Open a file for reading; caller closes. Returns empty File on failure.
  File openRead(const String& absPath);
  // Open a file for writing (truncate); caller closes. Returns empty File on failure.
  File openWrite(const String& absPath, FsResult& resOut);

  static const char* resultString(FsResult r);

  // Recursively delete upload debris: any file ending in ".partial" or any
  // zero-byte regular file (orphans from aborted uploads). Called at boot
  // for flash and after async SD mount.
  static void purgeUploadDebris(fs::FS& fs, const char* root = "/");

 private:
  std::vector<std::unique_ptr<FileSystemBackend>> _backends;
  std::vector<String> _readOnlyPrefixes;

  static bool sanitize(const String& absPath, String& cleaned);
  static bool removeRecursive(fs::FS& fs, const String& path);
};

#endif  // FileSystemManager_h
