#pragma once
#ifndef ESPRACK_MQTT_MODULE_H
#define ESPRACK_MQTT_MODULE_H

#include <Module.h>
#include <App.h>
#include "MqttSettingsService.h"
#include <memory>

class MqttModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "mqtt"; d.version = "2.0.0"; d.priority = 40;
    d.requires_ = {"wifi"};
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new MqttSettingsService(ctx.cfgMgr));
    // Late-bind IMqttProvider into App so any later-installed
    // service (Builder topo-sorts on priority + requires_) can
    // ctx.app->mqtt()->subscribe(...) in its onBegin. Mirrors the
    // TelegramModule wiring.
    if (ctx.app) ctx.app->setMqtt(svc_.get());
    svc_->registerManifest(ctx.web);
  }
  void onBegin() override { if (svc_) svc_->begin(); }
  void onLoop()  override { if (svc_) svc_->loop();  }

  // Service accessor — useful for App-level integration tests / direct
  // legacy callback registration.
  MqttSettingsService* service() { return svc_.get(); }
 private:
  std::unique_ptr<MqttSettingsService> svc_;
};

#endif
