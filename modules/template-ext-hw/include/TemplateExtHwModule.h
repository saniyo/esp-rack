#pragma once
#ifndef ESPRACK_TEMPLATE_EXT_HW_MODULE_H
#define ESPRACK_TEMPLATE_EXT_HW_MODULE_H

// TemplateExtHwModule — the copy-and-rename starting point for an EXTERNAL
// HW module (LIB-FEAT-001; walk-through: docs/module-guide.md "External HW
// modules"). A real but tiny driver: one output pin (relay, LED) and an
// optional push button that toggles it. Installed as instances:
//
//   .installExtHw<TemplateExtHwModule>("relay1", /*outPin=*/4, /*buttonPin=*/9)
//   .installExtHw<TemplateExtHwModule>("relay2", /*outPin=*/5)
//
// What an instance shows:
//   * its id names everything it registers: UI feature + WS channel
//     "<id>", /config/<id>.json, action "<id>.toggle"
//   * describeHw(): the pins it uses NOW (settings, -1 = none)
//   * settings writes are checked with hw()->validate() before they apply:
//     a reserved pin or another instance's pin is refused with the reason
//   * hwState() Conflict => the pins are left alone
//   * queryInterface(): other code finds it as ITemplateOutput —
//       app->hw()->find<ITemplateOutput>("relay1")->setOutput(true);
//   * restoreFactoryHw(): back to the ctor pins (System -> Hardware)
//
// The factory pins are the ctor args — the consumer knows the board.

#include <hw/ExtHwModule.h>
#include <hw/HwRegistry.h>
#include <memory>

#include "TemplateExtHwService.h"

// Example interface. A real driver's interface lives in its own header
// (shared by the drivers implementing it); the id is versioned: bump it on
// an incompatible change and old callers get nullptr instead of UB.
class ITemplateOutput {
 public:
  static constexpr const char* kIfaceId = "templateOutput.v1";
  // false when the instance can't drive its pin (Conflict, disabled, no pin)
  virtual bool setOutput(bool on) = 0;
  virtual bool output() const = 0;

 protected:
  ~ITemplateOutput() = default;
};

class TemplateExtHwModule : public ESPRack::ExtHwModule, public ITemplateOutput {
 public:
  TemplateExtHwModule(const char* instanceId, int outPin, int buttonPin = -1)
      : ESPRack::ExtHwModule(instanceId), factory_{outPin, buttonPin} {}

  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id       = instanceId();  // the instance id IS the module id
    d.version  = "0.1.1";
    d.priority = 60;
  }

  const char* typeName() const override { return "template-ext-hw"; }

  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new TemplateExtHwService(*this, hw(), ctx.cfgMgr, factory_));
    svc_->registerManifest(ctx.web);
  }

  // Boot pass (before onBegin) and hw()->update(): load the settings if
  // they aren't yet, then report the pins.
  void describeHw(ESPRack::HwDescriptor& d) const override {
    if (svc_) svc_->describeHw(d);
  }

  void* queryInterface(const char* ifaceId) override {
    if (ESPRack::ifaceIs(ifaceId, ITemplateOutput::kIfaceId)) return static_cast<ITemplateOutput*>(this);
    return nullptr;
  }

  bool restoreFactoryHw() override { return svc_ && svc_->restoreFactoryPins(); }

  bool setOutput(bool on) override { return svc_ && svc_->setOutput(on); }
  bool output() const override { return svc_ && svc_->output(); }

  void onBegin() override    { if (svc_) svc_->begin(); }
  void onLoop() override     { if (svc_) svc_->loop(); }
  void onShutdown() override { if (svc_) svc_->end(); }

 private:
  const TemplateExtHwPins factory_;
  std::unique_ptr<TemplateExtHwService> svc_;
};

#endif  // ESPRACK_TEMPLATE_EXT_HW_MODULE_H
