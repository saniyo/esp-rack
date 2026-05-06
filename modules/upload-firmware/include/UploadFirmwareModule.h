#pragma once
#ifndef ESPRACK_UPLOAD_FIRMWARE_MODULE_H
#define ESPRACK_UPLOAD_FIRMWARE_MODULE_H

#include <Module.h>
#include "UploadFirmwareService.h"
#include <memory>

class UploadFirmwareModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "uploadFirmware"; d.version = "1.0.0"; d.priority = 40;
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new UploadFirmwareService(ctx.server, ctx.security));
    svc_->registerManifest(ctx.web);
  }
 private:
  std::unique_ptr<UploadFirmwareService> svc_;
};

#endif
