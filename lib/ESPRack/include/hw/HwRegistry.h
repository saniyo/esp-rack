#pragma once
#ifndef ESPRACK_HW_REGISTRY_H
#define ESPRACK_HW_REGISTRY_H

// HwRegistry — app->hw() (LIB-FEAT-001): the board's reserved pins, the
// consumer's/framework's reservations and the live hardware claims of every
// external HW module instance (hw/ExtHwModule.h). Holds no persistent
// state: claims come from the instances' own settings.
//
// Threading: validate()/update()/state() may run on any task (settings
// updaters run on async_tcp); the claim table is guarded by a leaf mutex —
// the registry never calls module code while holding it.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <string.h>
#include <vector>

#include "hw/ExtHwModule.h"
#include "hw/HwClaims.h"

class WebManager;

namespace ESPRack {

class HwRegistry {
 public:
  HwRegistry();
  ~HwRegistry();
  HwRegistry(const HwRegistry&) = delete;
  HwRegistry& operator=(const HwRegistry&) = delete;

  // Would `who` holding exactly `proposed` be valid against the board and
  // every OTHER instance? Call from a settings updater before applying pins.
  HwCheck validate(const ExtHwModule& who, const HwDescriptor& proposed) const;

  // After `who` applied new settings: re-read its describeHw(), re-check,
  // update its claims and state. Returns the new state.
  HwState update(ExtHwModule& who);

  HwState state(const ExtHwModule& who) const;
  // The state's reason ("" when none): the conflict, the missing resource,
  // or a warning on an Ok instance (strapping pin, ADC2 with Wi-Fi).
  void stateReason(const ExtHwModule& who, char* buf, size_t len) const;

  // An instance by id, as interface I (nullptr when there's no such
  // instance or it doesn't implement I). Instances in Conflict are
  // returned too: they report their own state.
  template <class I>
  I* find(const char* instanceId) const {
    ExtHwModule* m = findInstance(instanceId);
    return m ? static_cast<I*>(m->queryInterface(I::kIfaceId)) : nullptr;
  }
  // fn(I&, const char* instanceId) for every instance implementing I, in
  // install order.
  template <class I, class Fn>
  void forEach(Fn fn) const {
    for (const Inst& x : _inst) {
      if (void* p = x.m->queryInterface(I::kIfaceId)) fn(*static_cast<I*>(p), x.m->instanceId());
    }
  }

  // Keep a GPIO away from every instance (a board's SD card, a status
  // LED, a consumer's own pin). Call before the boot pass: from
  // Builder::reserveGpio() or a framework module's onInstall. `reason` is
  // a static literal. false: no such GPIO on this chip, or the table is full.
  bool reserveGpio(int8_t pin, const char* reason);
  // A reserved GPIO (flash, PSRAM, console, reserveGpio())?
  bool reserved(int gpio, const char** reason = nullptr) const;

  // Does another instance claim this GPIO (pin kinds) or this bus/channel
  // now? Ask before releasing a pin you drove: it may have been moved to
  // someone else in the meantime.
  bool heldByOther(const ExtHwModule& who, HwKind kind, int index) const;

  HwChip chip() const { return _claims.board().chip; }
  const char* chipName() const { return _claims.board().chipName; }
  size_t instanceCount() const { return _inst.size(); }

 private:
  friend class App;
  friend class Builder;

  struct Inst {
    ExtHwModule* m;
    const char* type;     // typeName(), read outside the lock
    const char* version;  // describe().version, read at boot
    HwState state;
    HwDescriptor desc;    // as last described
    char reason[sizeof(HwCheck::reason)];
  };
  struct Rejected {
    char id[ExtHwModule::kIdMax + 1];
    char type[24];        // copied: the module is destroyed
    const char* version;
    const char* why;  // static literal
  };

  void addInstance(ExtHwModule* m);  // Builder, before the instance's onInstall
  void addRejected(const ExtHwModule& m, const char* version, const char* why);
  void boot();                       // App::begin, before every onBegin
  void registerManifest(WebManager* web);
  void loop();                       // App::loop: a pending "restore factory pins"

  ExtHwModule* findInstance(const char* id) const;
  Inst* instOf(const ExtHwModule& who);
  const Inst* instOf(const ExtHwModule& who) const;
  void evaluate(Inst& x, const HwDescriptor& d);  // under _mx
  void buildForm(JsonObject& root) const;
  void lock() const { xSemaphoreTake(_mx, portMAX_DELAY); }
  void unlock() const { xSemaphoreGive(_mx); }

  SemaphoreHandle_t _mx;
  HwClaims _claims;
  std::vector<Inst> _inst;  // install order; fixed after Builder::build()
  std::vector<Rejected> _rejected;
  std::atomic<bool> _restorePending{false};
};

// For ExtHwModule::queryInterface(): ifaceIs(ifaceId, IEnergyMeter::kIfaceId).
inline bool ifaceIs(const char* asked, const char* mine) { return asked && mine && strcmp(asked, mine) == 0; }

}  // namespace ESPRack

#endif  // ESPRACK_HW_REGISTRY_H
