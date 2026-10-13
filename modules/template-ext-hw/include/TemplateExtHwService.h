#pragma once
#ifndef ESPRACK_TEMPLATE_EXT_HW_SERVICE_H
#define ESPRACK_TEMPLATE_EXT_HW_SERVICE_H

// TemplateExtHwService — the working half of one TemplateExtHwModule
// instance. Same shape as modules/template (state struct + ConfigDelegate +
// typed feature + action), plus the instance parts:
//
//   * every id/path is derived from the instance id and kept in this
//     object (registrations hold pointers, so the buffers must live on)
//   * the config file is loaded with the plain updater (saved pins apply
//     verbatim; the boot pass decides Conflict), web writes go through
//     updateChecked(), which asks the registry first
//   * hardware is touched only from loop(), and only in HwState::Ok
//
// Threading as in modules/template: REST/WS writes on async_tcp, loop() on
// loopTask, shared data in _state behind read()/update*().

#include <Arduino.h>
#include <atomic>

#include <ConfigDelegate.h>
#include <FormBuilder.h>
#include <StatefulService.h>
#include <WebFeatureDelegate.h>
#include <hw/ExtHwModule.h>
#include <hw/HwRegistry.h>
#include <web/Web.h>

class WebManager;

struct TemplateExtHwPins {
  int outPin;
  int buttonPin;  // -1 = no button
};

class TemplateExtHwState {
 public:
  // ---- persisted settings ----
  bool enabled   = true;
  int  outPin    = -1;
  int  buttonPin = -1;
  bool activeLow = false;
  bool on        = false;   // the output survives a restart

  // ---- runtime ----
  ESPRack::HwState hwState = ESPRack::HwState::NotConfigured;
  char hwReason[sizeof(ESPRack::HwCheck::reason)] = {0};

  // The pins of THESE settings — for describeHw() and for checking a
  // proposal before it applies.
  void describeHw(ESPRack::HwDescriptor& d) const;

  static void readConfig(TemplateExtHwState& s, JsonObject& root);
  static StateUpdateResult update(JsonObject& root, TemplateExtHwState& s);
  static void buildForm(TemplateExtHwState& s, JsonObject& root);
  static void readState(TemplateExtHwState& s, JsonObject& root);
};

class TemplateExtHwService : public StatefulService<TemplateExtHwState> {
 public:
  TemplateExtHwService(ESPRack::ExtHwModule& owner, ESPRack::HwRegistry* hw, ConfigManager* cfgMgr,
                       const TemplateExtHwPins& factory);

  void registerManifest(WebManager* web);  // onInstall
  void begin();                            // onBegin
  void loop();                             // onLoop
  void end();                              // onShutdown

  void describeHw(ESPRack::HwDescriptor& d);  // loads the config first
  bool restoreFactoryPins();
  bool setOutput(bool on);
  bool output();

 private:
  StateUpdateResult updateChecked(JsonObject& root, TemplateExtHwState& s);
  void refreshHwState();  // registry -> _state (for the UI)
  void applyPins();       // loop() only
  void pollButton();      // loop() only

  ESPRack::ExtHwModule& _owner;
  ESPRack::HwRegistry* _hw;
  const TemplateExtHwPins _factory;
  ConfigDelegate<TemplateExtHwState> _cfg;
  WebFeatureEntry<TemplateExtHwState>* _feature{nullptr};
  std::atomic<bool> _applyPending{false};

  // Registration strings (the id is ESPRack::ExtHwModule::instanceId()).
  char _wsPath[4 + ESPRack::ExtHwModule::kIdMax + 1];
  char _toggleId[ESPRack::ExtHwModule::kIdMax + sizeof(".toggle")];
  char _title[sizeof("Output ") + ESPRack::ExtHwModule::kIdMax];

  // Pins as driven now — loop() only.
  int _drivenOut{-1}, _drivenButton{-1};
  bool _lastButton{true};
  uint32_t _buttonChangeMs{0};
};

#endif  // ESPRACK_TEMPLATE_EXT_HW_SERVICE_H
