#ifndef FileSystemBackend_h
#define FileSystemBackend_h

#include <Arduino.h>
#include <FS.h>

class FileSystemBackend {
 public:
  virtual ~FileSystemBackend() = default;

  // volume name used as first path segment, e.g. "flash", "sd"
  virtual const char* name() const = 0;

  virtual bool isMounted() const = 0;
  virtual bool mount() = 0;
  // Non-blocking variant: kicks a background retry loop and returns immediately.
  // Default = synchronous mount() (backends without a flaky power-up window).
  virtual bool mountAsync() { return mount(); }
  virtual void unmount() = 0;

  virtual bool readOnly() const { return false; }
  virtual bool canFormat() const { return false; }
  virtual bool format() { return false; }

  // Format progress reporting (default: idle, 0%).
  // formatState: 0=idle, 1=counting, 2=running, 3=done, 4=error
  virtual uint8_t formatState() const { return 0; }
  virtual int formatProgress() const { return 0; }
  virtual uint32_t formatTotal() const { return 0; }
  virtual uint32_t formatDone() const { return 0; }

  virtual uint64_t totalBytes() const = 0;
  virtual uint64_t usedBytes() const = 0;

  // returns nullptr if not mounted
  virtual fs::FS* fs() = 0;
};

#endif  // FileSystemBackend_h
