#pragma once
#ifndef ESPRACK_MODULE_H
#define ESPRACK_MODULE_H

// Module — the plug-in contract every ESPRack-aware feature implements.
//
// A module is constructed by the consumer (with any hardware-specific
// args), then handed to ESPRack::Builder.install<>(). Builder validates
// dependencies, sorts by priority + topological order, and walks the
// 5-phase lifecycle:
//
//   describe(d)   — pure metadata. Builder reads BEFORE construction
//                   side-effects to plan install order. Module fills
//                   `d` with id, version, dependencies, priority.
//
//   onInstall(c)  — register concerns: cfg.attach() for persistence,
//                   web->registerFeature() for UI manifest, addUpdate
//                   Handlers, etc. WiFi/peripherals are NOT YET UP.
//                   Called once, in topological install order.
//
//   onBegin()     — post-network init. WiFi.mode(AP_STA) has run. Safe
//                   to start sockets, mount SD, init sensors. Default
//                   is no-op.
//
//   onLoop()      — cooperative tick. Called every iteration of the
//                   consumer's main loop(). Must NOT block. Default
//                   no-op.
//
//   onShutdown()  — graceful cleanup before factory reset / restart.
//                   Called in REVERSE install order so dependents tear
//                   down first. Default no-op.
//
// See ROADMAP.md §3 for the full contract and §3.3 for dependency
// resolution semantics.

#include <Arduino.h>
#include <vector>

class AsyncWebServer;
class ConfigManager;
class WebManager;
class WsManager;
class SecurityManager;

namespace ESPRack {

// Framework singletons handed to a module during onInstall. Pointers
// may be nullptr when the corresponding subsystem is compiled out via
// build flag (e.g. WsManager when ESPRACK_WEBSOCKET=0). Modules MUST
// guard against nullptr where they actually use a particular
// subsystem.
struct ModuleContext {
  AsyncWebServer*  server   {nullptr};
  ConfigManager*   cfgMgr   {nullptr};
  WebManager*      web      {nullptr};
  WsManager*       ws       {nullptr};
  SecurityManager* security {nullptr};
};

// Self-description filled in by Module::describe(). Builder consumes
// this BEFORE onInstall to compute install order, validate
// dependencies, and surface helpful "X requires Y but Y not installed"
// errors. Lifetime: descriptor is held by Builder while modules are
// being installed; after build() returns, individual modules' state
// holds whatever they need.
struct ModuleDescriptor {
  // Stable identifier used as the dependency key. Must be unique
  // across all installed modules. Convention: camelCase, e.g. "wifi",
  // "lightControl", "mqtt". String must outlive the module (use a
  // static literal).
  const char* id {nullptr};

  // Informational version string, "1.0.0" style. Surfaced in logs and
  // optionally in the UI's About panel. Not used for dependency
  // resolution.
  const char* version {"0.0.0"};

  // Dependency module ids. Builder verifies every entry here matches
  // a peer module's id and that no cycle exists. Entries are static
  // string literals (must outlive the module).
  std::vector<const char*> requires_ {};

  // Install order tiebreaker within the topological sort. 0..100, lower
  // = earlier. Defaults to 50. Use lower for low-level (security,
  // filesystem); higher for app code (modules that surface UI).
  int priority {50};

  // Explicit opt-out used by build-flag gates. Builder skips installation
  // when false. Default true.
  bool enabled {true};
};

// Module — abstract base every plug-in inherits from. Lifecycle is
// driven by ESPRack::App; consumers never call these directly.
class Module {
 public:
  virtual ~Module() = default;

  // Phase A. Pure introspection — fill the descriptor and return.
  // Builder calls this BEFORE onInstall, possibly several times during
  // dependency analysis. Implementations must be idempotent and side-
  // effect free.
  virtual void describe(ModuleDescriptor& d) = 0;

  // Phase B. Wire into framework singletons. Called once, in
  // topological install order. Hardware peripherals NOT YET safe.
  virtual void onInstall(ModuleContext& ctx) = 0;

  // Phase C. Network + peripherals are alive. Default no-op.
  virtual void onBegin() {}

  // Phase D. Main loop tick. Default no-op.
  virtual void onLoop() {}

  // Phase E. Reverse-order shutdown. Default no-op.
  virtual void onShutdown() {}
};

}  // namespace ESPRack

#endif  // ESPRACK_MODULE_H
