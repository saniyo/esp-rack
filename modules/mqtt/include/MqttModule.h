#pragma once
#ifndef ESPRACK_MQTT_MODULE_H
#define ESPRACK_MQTT_MODULE_H

#include <Module.h>
#include "MqttSettingsService.h"
#include <memory>

class MqttModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "mqtt"; d.version = "1.0.0"; d.priority = 40;
    d.requires_ = {"wifi"};
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new MqttSettingsService(ctx.cfgMgr));
    svc_->registerManifest(ctx.web);
  }
  void onBegin() override { if (svc_) svc_->begin(); }
  void onLoop()  override { if (svc_) svc_->loop();  }
 private:
  std::unique_ptr<MqttSettingsService> svc_;
};

#endif
