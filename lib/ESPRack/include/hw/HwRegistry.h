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

// System -> Hardware tab. 1 (default): always shown (plan Q5). 0: the tab
// and its texts are compiled out (about 4.3 KB on C3); the registry, the
// claims and the admin action system.restoreFactoryHw stay.
#ifndef ESPRACK_HW_TAB
#define ESPRACK_HW_TAB 1
#endif

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
  // returned too: they report their own state. Takes no lock (the instance
  // list is fixed after Builder::build()), so describeHw() may call it.
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
  // fn(const char* instanceId, const HwResource& r, HwState state) for every
  // resource of `kind` in every instance's last describeHw(), in install
  // order (SES-LIB-034). Instances that don't hold it are listed too, with
  // their state: Conflict, Disabled, NotConfigured (r.index -1). An I2C bus
  // page lists its devices with HwKind::I2cAddr: the rows whose r.bus is
  // its controller (II2cBus.h). Any task: the rows are copied under the
  // lock and fn runs after it is released, so fn may call the registry.
  template <class Fn>
  void forEachClaim(HwKind kind, Fn fn) const {
    std::vector<ClaimRow> rows;
    claimsOf(kind, rows);
    for (const ClaimRow& c : rows) fn(c.id, c.res, c.state);
  }

  // SES-LIB-024: `who` has let go of the hardware it gave up (pins switched
  // back to input, UART ended). Until then that hardware stays claimed as
  // "releasing" and other instances can't take it. Call it from the loop
  // that switches the hardware (ExtHwModule::hwReleased()).
  void released(const ExtHwModule& who);

  // SES-LIB-025: a runtime note on `who` — Ok only means the resources are
  // free, not that the device answered. nullptr clears it. Any task.
  void setRuntimeNote(const ExtHwModule& who, const char* note, HwNoteLevel level = HwNoteLevel::Warn);

  // SES-LIB-023: keep any resource away from every instance — a UART, an
  // RMT/LEDC channel ... that the consumer or a framework module drives
  // itself. GPIO kinds go to reserveGpio(). Before the boot pass; `reason`
  // is a static literal.
  bool reserve(HwKind kind, int16_t index, const char* reason, int8_t bus = -1);

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
    char note[48];        // SES-LIB-025 runtime note ("" = none)
    HwNoteLevel noteLevel;
  };
  void manifestItem(size_t i, JsonObject o, const WebManager& web) const;  // extHw[i]
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

  // The UI label of the external HW instances: the System tab and the menu
  // groups (the UI reads it from that tab's title). SES-LIB-028.
  static constexpr const char* kLabel = "External Hardware";
  void loop();                       // App::loop: a pending "restore factory pins"

  ExtHwModule* findInstance(const char* id) const;
  struct ClaimRow {
    const char* id;  // the module's instanceId() buffer
    HwResource res;
    HwState state;
  };
  void claimsOf(HwKind kind, std::vector<ClaimRow>& out) const;  // forEachClaim: copies under the lock
  Inst* instOf(const ExtHwModule& who);
  const Inst* instOf(const ExtHwModule& who) const;
  void evaluate(Inst& x, const HwDescriptor& d);  // under _mx
  // System -> Hardware (SES-LIB-033): the form (values = false: the schema,
  // no rows) and its flat values for GET .../state.
  void buildForm(JsonObject& root, bool values) const;
  void buildState(JsonObject& root) const;
  // Under the lock.
  void chipText(char* buf, size_t len) const;
  void budgetText(char* buf, size_t len) const;
  void fillInstanceRows(JsonArray rows) const;
  void fillPinRows(JsonArray rows) const;
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
