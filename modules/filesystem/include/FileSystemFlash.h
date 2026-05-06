#ifndef FileSystemFlash_h
#define FileSystemFlash_h

#include <ESPFS.h>
#include <LittleFS.h>

#include "FileSystemBackend.h"

class FileSystemFlash : public FileSystemBackend {
 public:
  const char* name() const override { return "flash"; }

  bool isMounted() const override { return _mounted; }

  bool mount() override {
    if (_mounted) return true;
    _mounted = ESPFS.begin(true);
    return _mounted;
  }

  void unmount() override {
    if (_mounted) {
      ESPFS.end();
      _mounted = false;
    }
  }

  uint64_t totalBytes() const override { return _mounted ? ESPFS.totalBytes() : 0; }
  uint64_t usedBytes() const override { return _mounted ? ESPFS.usedBytes() : 0; }

  fs::FS* fs() override { return _mounted ? static_cast<fs::FS*>(&ESPFS) : nullptr; }

  // Framework mounts LittleFS inside ESPReact::begin(); call this afterwards to
  // mark the backend as already mounted without double-begin.
  void adoptMounted() { _mounted = true; }

 private:
  bool _mounted{false};
};

#endif  // FileSystemFlash_h
