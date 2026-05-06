#pragma once
#ifndef ESPRACK_FILE_SYSTEM_MODULE_H
#define ESPRACK_FILE_SYSTEM_MODULE_H

// FileSystemModule — owns BOTH the FileSystemManager singleton and
// the HTTP-facing FileSystemService. Backends (LittleFS flash + SD)
// are registered in onBegin so the radio/peripherals are alive.

#include <Module.h>
#include <ESPFS.h>
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
    d.id = "filesystem"; d.version = "1.0.0"; d.priority = 20;
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
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
  std::unique_ptr<FileSystemManager> manager_;
  std::unique_ptr<FileSystemService> service_;
};

#endif
