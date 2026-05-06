#ifndef FileSystemSd_h
#define FileSystemSd_h

#include <atomic>
#include "FileSystemBackend.h"
#include "FileSystemManager.h"

#ifdef BOARD_HAS_SD

#if defined(BOARD_SD_MMC) || !defined(BOARD_SD_SPI)
#include <SD_MMC.h>
#define FS_SD_DRIVER SD_MMC
#else
#include <SD.h>
#include <SPI.h>
#define FS_SD_DRIVER SD
#endif

class FileSystemSd : public FileSystemBackend {
 public:
  const char* name() const override { return "sd"; }

  bool isMounted() const override { return _mounted; }

  bool mount() override {
    if (_mounted) return true;
#if defined(BOARD_SD_MMC) || !defined(BOARD_SD_SPI)
#if defined(BOARD_SD_MMC_CLK) && defined(BOARD_SD_MMC_CMD) && defined(BOARD_SD_MMC_D0)
#ifdef BOARD_SD_MMC_INTERNAL_PULLUPS
    // ~45 kΩ internal pull-ups on CMD/D0 (CLK doesn't need one).
    pinMode(BOARD_SD_MMC_CMD, INPUT_PULLUP);
    pinMode(BOARD_SD_MMC_D0,  INPUT_PULLUP);
#endif
    // ESP32-S3 has no fixed SDMMC pin mapping — assign before begin().
    SD_MMC.setPins(BOARD_SD_MMC_CLK, BOARD_SD_MMC_CMD, BOARD_SD_MMC_D0);
#endif
#ifdef BOARD_SD_MMC_1BIT
    _mounted = SD_MMC.begin("/sdcard", true);  // 1-bit mode
#else
    _mounted = SD_MMC.begin();                  // 4-bit default
#endif
    if (_mounted) {
      Serial.printf("[SD_MMC] mounted, type=%d, size=%llu MB\n",
                    SD_MMC.cardType(), SD_MMC.cardSize() / (1024ULL * 1024));
    } else {
      SD_MMC.end();
    }
#else
#ifndef SD_CS_PIN
#define SD_CS_PIN SS
#endif
#if defined(BOARD_SD_SPI_SCK) && defined(BOARD_SD_SPI_MISO) && defined(BOARD_SD_SPI_MOSI)
    // ESP32-S3 has no fixed SPI pin mapping — assign before SD.begin().
    SPI.begin(BOARD_SD_SPI_SCK, BOARD_SD_SPI_MISO, BOARD_SD_SPI_MOSI, SD_CS_PIN);
#ifndef BOARD_SD_SPI_FREQ
#define BOARD_SD_SPI_FREQ 20000000U  // 20 MHz default; bump if signals are clean
#endif
    _mounted = SD.begin(SD_CS_PIN, SPI, BOARD_SD_SPI_FREQ);
#else
    _mounted = SD.begin(SD_CS_PIN);
#endif
    if (_mounted) {
      Serial.printf("[SD] mounted, type=%d, size=%llu MB\n",
                    SD.cardType(), SD.cardSize() / (1024ULL * 1024));
    } else {
      SD.end();
    }
#endif
    return _mounted;
  }

  // Returns immediately. Kicks a one-shot background task that retries
  // SD.begin() a few times (with vTaskDelay between attempts) — covers the
  // card's power-up window without blocking the caller (e.g. async_tcp).
  // Idempotent: if already mounted or a watcher is in flight, it's a no-op.
  bool mountAsync() {
    if (_mounted) return true;
    bool expected = false;
    if (!_watcherActive.compare_exchange_strong(expected, true)) return true;
    BaseType_t r = xTaskCreatePinnedToCore(
        mountWatcherTask, "sd_mount_w", 8192, this, 1, nullptr, 0);
    if (r != pdPASS) {
      _watcherActive = false;
      return false;
    }
    return true;
  }

  void unmount() override {
    if (!_mounted) return;
#if defined(BOARD_SD_MMC) || !defined(BOARD_SD_SPI)
    SD_MMC.end();
#else
    SD.end();
#endif
    _mounted = false;
  }

  uint64_t totalBytes() const override { return _mounted ? FS_SD_DRIVER.totalBytes() : 0; }
  uint64_t usedBytes() const override { return _mounted ? FS_SD_DRIVER.usedBytes() : 0; }

  fs::FS* fs() override { return _mounted ? static_cast<fs::FS*>(&FS_SD_DRIVER) : nullptr; }

  bool canFormat() const override { return true; }

  uint8_t formatState() const override { return _formatState; }
  int formatProgress() const override {
    uint32_t total = _formatTotal;
    if (total == 0) return _formatState >= 3 ? 100 : 0;
    uint32_t done = _formatDone;
    if (done >= total) return 100;
    return (int)((done * 100) / total);
  }
  uint32_t formatTotal() const override { return _formatTotal; }
  uint32_t formatDone() const override { return _formatDone; }

  bool format() override {
    if (!_mounted) {
      Serial.println("[SD] format failed — not mounted");
      _formatState = 4;
      return false;
    }

    _formatState = 1;   // counting
    _formatTotal = 0;
    _formatDone = 0;

    fs::FS* f = &FS_SD_DRIVER;

    // pass 1: count entries for progress reporting
    File root = f->open("/");
    uint32_t total = 0;
    if (root && root.isDirectory()) {
      File entry = root.openNextFile();
      while (entry) {
        String name = String("/") + entry.name();
        bool isDir = entry.isDirectory();
        entry.close();
        if (isDir) total += countRecursive(*f, name);
        total++;  // entry itself
        vTaskDelay(1);
        entry = root.openNextFile();
      }
      root.close();
    }
    _formatTotal = total;
    Serial.printf("[SD] format: %lu entries to delete\n", (unsigned long)total);
    _formatState = 2;   // running

    // pass 2: delete with progress
    root = f->open("/");
    if (root && root.isDirectory()) {
      std::vector<String> names;
      File entry = root.openNextFile();
      while (entry) {
        names.push_back(String("/") + entry.name());
        entry = root.openNextFile();
      }
      root.close();
      for (auto& n : names) {
        removeRecursive(*f, n);
        vTaskDelay(1);
      }
    }
    _formatState = 3;   // done
    Serial.println("[SD] format complete (all files removed)");
    return true;
  }

 private:
  volatile bool _mounted{false};
  std::atomic<bool> _watcherActive{false};
  // 32-bit aligned reads/writes are atomic on ESP32 — volatile is sufficient.
  volatile uint8_t  _formatState{0};
  volatile uint32_t _formatTotal{0};
  volatile uint32_t _formatDone{0};

  static void mountWatcherTask(void* arg) {
    auto* self = static_cast<FileSystemSd*>(arg);
    // 5 attempts spread over ~1.5 s — covers card power-up; yields between tries.
    for (int i = 1; i <= 5 && !self->_mounted; i++) {
      if (self->mount()) break;
      Serial.printf("[SD] async mount attempt %d failed; retrying…\n", i);
      vTaskDelay(pdMS_TO_TICKS(150 * i));  // 150, 300, 450, 600, 750
    }
    if (self->_mounted) {
      if (auto* f = self->fs()) FileSystemManager::purgeUploadDebris(*f);
    } else {
      Serial.println("[SD] async mount gave up after 5 attempts");
    }
    self->_watcherActive = false;
    vTaskDelete(nullptr);
  }

  static uint32_t countRecursive(fs::FS& vfs, const String& path) {
    File f = vfs.open(path);
    if (!f) return 0;
    uint32_t n = 0;
    if (f.isDirectory()) {
      File child = f.openNextFile();
      while (child) {
        String cp = path + "/" + child.name();
        bool isDir = child.isDirectory();
        child.close();
        if (isDir) n += countRecursive(vfs, cp);
        n++;  // entry itself
        vTaskDelay(1);
        child = f.openNextFile();
      }
      f.close();
    } else {
      f.close();
      n = 1;
    }
    return n;
  }

  void removeRecursive(fs::FS& vfs, const String& path) {
    File f = vfs.open(path);
    if (!f) return;
    if (f.isDirectory()) {
      File child = f.openNextFile();
      while (child) {
        String cp = path + "/" + child.name();
        bool isDir = child.isDirectory();
        child.close();
        if (isDir) removeRecursive(vfs, cp);
        else { vfs.remove(cp.c_str()); _formatDone++; }
        vTaskDelay(1);
        child = f.openNextFile();
      }
      f.close();
      vfs.rmdir(path.c_str());
      _formatDone++;
    } else {
      f.close();
      vfs.remove(path.c_str());
      _formatDone++;
    }
  }
};

#else  // !BOARD_HAS_SD — stub so that FileSystemManager can always reference the type

class FileSystemSd : public FileSystemBackend {
 public:
  const char* name() const override { return "sd"; }
  bool isMounted() const override { return false; }
  bool mount() override { return false; }
  void unmount() override {}
  uint64_t totalBytes() const override { return 0; }
  uint64_t usedBytes() const override { return 0; }
  fs::FS* fs() override { return nullptr; }
};

#endif  // BOARD_HAS_SD

#endif  // FileSystemSd_h
