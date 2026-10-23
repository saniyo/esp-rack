#pragma once
#ifndef ESPRACK_HW_EXT_HW_MODULE_H
#define ESPRACK_HW_EXT_HW_MODULE_H

// ExtHwModule — base class for an external hardware module INSTANCE
// (LIB-FEAT-001): the same driver may be installed several times on
// different pins, each with its own id, settings file, UI feature and WS
// channel.
//
//   .installExtHw<MeterModule>("meter1", Serial1)
//   .installExtHw<MeterModule>("meter2", Serial2)
//
// The instance tells the framework which hardware it uses NOW
// (describeHw(), from its settings); the HwRegistry (app->hw()) checks it
// against the board's reserved pins and every other instance:
//
//   * boot: after every onInstall and ConfigManager::begin(), before any
//     onBegin, in install order — the first claimer of a resource wins. A
//     later instance that collides (or sits on a reserved pin) is
//     HwState::Conflict and must not touch hardware; one with a required
//     resource at -1 is NotConfigured.
//   * settings change: the updater calls hw()->validate(*this, proposed)
//     and returns StateUpdateResult::ERROR (with setUpdateError(reason))
//     when it isn't ok; after applying it calls hw()->update(*this).
//
// Lookup without a typed App accessor per module type:
//   app->hw()->find<IEnergyMeter>("meter1"), forEach<IEnergyMeter>(fn)
// through queryInterface(I::kIfaceId) — versioned string ids
// ("energyMeter.v1"), compared by content, no RTTI.
//
// Walk-through: docs/module-guide.md "External HW modules";
// example: modules/template-ext-hw.

#include <Arduino.h>

#include "Module.h"
#include "hw/HwResource.h"

class WebMenu;  // WebFeatureBuilder.h

namespace ESPRack {

class HwRegistry;

class ExtHwModule : public Module {
 public:
  static constexpr size_t kIdMax = 23;

  // instanceId is copied: [a-z0-9_-], 1..23 chars, unique among all
  // modules, not "system". An invalid id is reported by Builder and the
  // instance is not installed.
  explicit ExtHwModule(const char* instanceId);

  const char* instanceId() const { return _id; }
  bool instanceIdValid() const { return _idValid; }

  // Resources this instance uses NOW, from its settings (-1 = not
  // configured). Called on the loop task at boot and from update() — never
  // with a registry lock held, but it must not call into the registry
  // (except find(), which takes no lock: i2cDeviceClaim() uses it).
  // The boot call comes before onBegin: load the settings here if they
  // aren't loaded yet (ConfigDelegate::ensureLoaded()).
  virtual void describeHw(HwDescriptor& d) const = 0;

  // The implemented interface for `ifaceId` (I::kIfaceId), or nullptr.
  void* queryInterface(const char* ifaceId) override {
    (void)ifaceId;
    return nullptr;
  }

  // Driver type shown in System -> Hardware as typeName@version ("meter").
  virtual const char* typeName() const = 0;

  // Optional recovery: move this instance back to its factory pins and
  // save. System -> Hardware "Restore factory pins" calls it on every
  // instance (loop task), then restarts. Return false when unsupported.
  virtual bool restoreFactoryHw() { return false; }

  ExtHwModule* asExtHw() override { return this; }

  // Naming convention for everything an instance registers — the id keeps
  // instances apart. The returned buffers live as long as the module.
  const char* featureId() const { return _id; }  // UI feature + WS channel
  const char* wsChannel() const { return _id; }
  // ConfigManager id and file: "hw-<id>", "/config/hw-<id>.json" — a prefix
  // of their own, so an instance never shares a framework module's file.
  const char* configId() const { return _cfgId; }
  const char* configPath() const { return _path; }
  // "<id>.<verb>" into buf (e.g. a manifest action id); keep buf alive as
  // long as the registration that points at it.
  const char* actionId(const char* verb, char* buf, size_t len) const;

  // SES-LIB-028. The module that uses this instance (Builder::owner()), or
  // nullptr. Its pages then sit in an "External Hardware" group under the
  // owner's menu entry and say "Used by <owner>". hwOwned(): hide manual
  // controls the owner drives itself.
  const char* hwOwner() const { return _hwOwner; }
  bool hwOwned() const { return _hwOwner != nullptr; }
  // The menu entry of an instance page. The UI places it from the registry
  // (under the owner, else under System), so never nestUnder() it; its
  // tabs stay in the page header.
  WebMenu hwMenu(const char* label) const;
  // The instance's title in the manifest's extHw[]; nullptr = its id.
  virtual const char* hwTitle() const { return nullptr; }
  // The interface ids queryInterface() answers, nullptr-terminated, for
  // extHw[]; nullptr = none listed.
  virtual const char* const* interfaceIds() const { return nullptr; }

 protected:
  HwRegistry* hw() const { return _hw; }  // set before onInstall
  HwState hwState() const;                // Ok / NotConfigured / Conflict / Disabled
  // After switching the hardware off the resources it gave up (from loop):
  // they stop blocking other instances (SES-LIB-024).
  void hwReleased();
  // Runtime state for System -> Hardware (SES-LIB-025); nullptr clears.
  void hwNote(const char* note, HwNoteLevel level = HwNoteLevel::Warn);

 private:
  friend class HwRegistry;
  friend class Builder;
  const char* _hwOwner = nullptr;  // a module id (Builder::owner())
  char _id[kIdMax + 1];
  char _cfgId[sizeof("hw-") + kIdMax];
  char _path[sizeof("/config/hw-") + kIdMax + sizeof(".json")];
  bool _idValid;
  HwRegistry* _hw = nullptr;
};

// Is `id` a valid instance id: 1..23 chars of [a-z0-9_-], not "system"
// (the framework's System feature)?
bool isValidInstanceId(const char* id);

}  // namespace ESPRack

#endif  // ESPRACK_HW_EXT_HW_MODULE_H
