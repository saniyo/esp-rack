#pragma once
#ifndef ESPRACK_FILE_SYSTEM_MODULE_H
#define ESPRACK_FILE_SYSTEM_MODULE_H

// FileSystemModule — owns BOTH the FileSystemManager singleton and
// the HTTP-facing FileSystemService. Backends (LittleFS flash + SD)
// are registered in onBegin so the radio/peripherals are alive.

#include <App.h>
#include <Module.h>
#include <ESPFS.h>
#include <hw/HwRegistry.h>
#include "FileSystemManager.h"
#include "FileSystemService.h"
#include "FileSystemFlash.h"
#ifdef BOARD_HAS_SD
#include "FileSystemSd.h"
#endif
#include <memory>

class FileSystemModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "filesystem"; d.version = "1.1.0"; d.priority = 20;
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    reserveSdPins(ctx.app ? ctx.app->hw() : nullptr);
    manager_.reset(new FileSystemManager());
    service_.reset(new FileSystemService(ctx.server, manager_.get(), ctx.security));
    service_->registerManifest(ctx.web);
  }
  void onBegin() override {
    auto flash = std::unique_ptr<FileSystemFlash>(new FileSystemFlash());
    flash->adoptMounted();
    FileSystemManager::purgeUploadDebris(ESPFS);
    manager_->addBackend(std::move(flash));
#ifdef BOARD_HAS_SD
    auto sd = std::unique_ptr<FileSystemSd>(new FileSystemSd());
    sd->mountAsync();
    manager_->addBackend(std::move(sd));
#endif
    manager_->addReadOnlyPrefix("/flash/config");
  }
 private:
  // The SD card's pins are the framework's: keep external HW modules off
  // them (LIB-FEAT-001). Only pins the board defines explicitly.
  static void reserveSdPins(ESPRack::HwRegistry* hw) {
    if (!hw) return;
#if defined(BOARD_HAS_SD) && (defined(BOARD_SD_MMC) || !defined(BOARD_SD_SPI))
#ifdef BOARD_SD_MMC_CLK
    hw->reserveGpio(BOARD_SD_MMC_CLK, "SD card (CLK)");
#endif
#ifdef BOARD_SD_MMC_CMD
    hw->reserveGpio(BOARD_SD_MMC_CMD, "SD card (CMD)");
#endif
#ifdef BOARD_SD_MMC_D0
    hw->reserveGpio(BOARD_SD_MMC_D0, "SD card (D0)");
#endif
#if !defined(BOARD_SD_MMC_1BIT) && defined(BOARD_SD_MMC_D1) && defined(BOARD_SD_MMC_D2) && defined(BOARD_SD_MMC_D3)
    hw->reserveGpio(BOARD_SD_MMC_D1, "SD card (D1)");
    hw->reserveGpio(BOARD_SD_MMC_D2, "SD card (D2)");
    hw->reserveGpio(BOARD_SD_MMC_D3, "SD card (D3)");
#endif
#endif
  }

  std::unique_ptr<FileSystemManager> manager_;
  std::unique_ptr<FileSystemService> service_;
};

#endif
