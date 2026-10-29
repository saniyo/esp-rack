#pragma once
#ifndef ESPRACK_TEMPLATE_MODULE_H
#define ESPRACK_TEMPLATE_MODULE_H

// TemplateModule — the copy-and-rename starting point for a new ESPRack
// module (walk-through: docs/module-guide.md). It is a small but real
// hardware module: a UART line monitor that counts bytes/lines on a serial
// port and shows them live in the UI.
//
// The Module class stays THIN: identity in describe(), wiring in
// onInstall(), and a one-line forward of each lifecycle phase to the
// service that does the work (TemplateService). Every built-in module
// follows this split (e.g. modules/ntp/include/NTPModule.h).
//
// Consumer:
//   .install<TemplateModule>(Serial2)   // which UART is the consumer's call;
//                                       // pins + baud are settings
//
// Hardware rule: the module never picks a UART or a pin itself. The port
// is injected through the ctor, pins/baud come from factory defaults
// (FACTORY_TEMPLATE_* in the consumer's factory_settings.ini) and can be
// changed in the UI at runtime.

#include <HardwareSerial.h>
#include <Module.h>
#include <memory>

#include "TemplateService.h"

class TemplateModule : public ESPRack::Module {
 public:
  explicit TemplateModule(HardwareSerial& port = Serial1) : port_(port) {}

  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id       = "template";   // unique; also the UI feature id + WS channel
    d.version  = "0.1.0";
    d.priority = 60;           // app-level module: after the framework (5..40)
    // d.requires_ = {"wifi"}; // HARD deps only — Builder refuses to order
    //                         // a module whose requirement isn't installed
  }

  void onInstall(ESPRack::ModuleContext& ctx) override {
    // Filesystem is NOT mounted yet and peripherals are not up: only
    // construct + register here. Loading config happens in onBegin().
    svc_.reset(new TemplateService(ctx.cfgMgr, port_));
    svc_->registerManifest(ctx.web);
  }

  void onBegin() override    { if (svc_) svc_->begin(); }
  void onLoop() override     { if (svc_) svc_->loop();  }
  // How long the loop may wait before onLoop must run again (its nearest
  // timer); flags set on other tasks call ESPRack::wakeLoop() instead.
  uint32_t loopWaitMs(uint32_t nowMs) const override {
    return svc_ ? svc_->loopWaitMs(nowMs) : ESPRACK_LOOP_IDLE_WAIT_MS;
  }
  void onShutdown() override { if (svc_) svc_->end();   }

 private:
  HardwareSerial& port_;
  std::unique_ptr<TemplateService> svc_;
};

#endif  // ESPRACK_TEMPLATE_MODULE_H
