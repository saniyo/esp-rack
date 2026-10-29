#ifndef FileSystemManager_h
#define FileSystemManager_h

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <vector>
#include <memory>

#include "FileSystemBackend.h"
#include "FsWriteQueue.h"

struct FileSystemEntry {
  String name;
  bool isDir;
  size_t size;
  uint32_t mtime;  // 0 if unavailable
};

// FsResult: FsWriteQueue.h

// The file primitives FsWriteQueue runs on (an fs::FS volume).
class FsWriteIo {
 public:
  using Vol = fs::FS*;
  using Str = String;
  FsWriteIo();
  bool lock(uint32_t ms);
  void unlock();
  bool canDefer() const { return _job >= 0; }
  void kick();
  void setJob(int8_t job) { _job = job; }
  int8_t kind(Vol fs, const String& path);
  bool exists(Vol fs, const String& path) { return fs->exists(path); }
  bool firstChild(Vol fs, const String& dir, String& child, bool& isDir);
  bool mkdir(Vol fs, const String& path) { return fs->mkdir(path); }
  bool remove(Vol fs, const String& path) { return fs->remove(path); }
  bool rmdir(Vol fs, const String& path) { return fs->rmdir(path); }
  bool rename(Vol fs, const String& from, const String& to) { return fs->rename(from, to); }

 private:
  SemaphoreHandle_t _mx = nullptr;
  int8_t _job = -1;     // FlashWriter
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
  // At once, outside the flash write gate (power/WriteGate.h): the caller
  // holds a gate pass or slot, or the volume is off the code flash.
  FsResult mkdir(const String& absPath);
  FsResult remove(const String& absPath, bool recursive);
  FsResult rename(const String& fromAbs, const String& toAbs);

  // ---- writes through the flash write gate (any task, never waits) ----
  // The result of mkdir()/remove()/rename() when the gate let the write go
  // at once (a free gate: always, as without it). QUEUED: put off, the
  // FlashWriter task does it, a gate slot per step (a tree: one removal per
  // step), in the order asked; *op: its number for writeStatus(). BUSY:
  // no room to keep it now. Volumes off the code flash (SD) go at once
  // (*op 0).
  FsResult mkdirGated(const String& absPath, uint32_t* op = nullptr);
  FsResult removeGated(const String& absPath, bool recursive, uint32_t* op = nullptr);
  FsResult renameGated(const String& fromAbs, const String& toAbs, uint32_t* op = nullptr);
  using WriteQueue = FsWriteQueue<FsWriteIo>;
  // false: no such write (any more).
  bool writeStatus(uint32_t op, WriteQueue::Status& out) { return _writes.status(op, out); }
  uint8_t writesPending() const { return _writes.pending(); }

  // Open a file for reading; caller closes. Returns empty File on failure.
  File openRead(const String& absPath);
  // Open a file for writing (truncate); caller closes. Returns empty File on failure.
  File openWrite(const String& absPath, FsResult& resOut);

  static const char* resultString(FsResult r);

  // Recursively delete upload debris: files ending in ".partial" ONLY.
  // Uploads write <target>.partial and promote it on success, so an
  // aborted upload leaves nothing else; zero-byte files are legitimate.
  // Called at boot for flash and after async SD mount.
  static void purgeUploadDebris(fs::FS& fs, const char* root = "/");

 private:
  std::vector<std::unique_ptr<FileSystemBackend>> _backends;
  std::vector<String> _readOnlyPrefixes;

  FsWriteIo _io;
  WriteQueue _writes{_io};

  static bool sanitize(const String& absPath, String& cleaned);
  // A volume the asker may write: OK and its fs and path, or why not.
  FsResult writable(const String& absPath, fs::FS*& fs, String& rel, FileSystemBackend** bOut = nullptr) const;
};

#endif  // FileSystemManager_h
