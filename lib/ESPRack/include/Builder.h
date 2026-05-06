#pragma once
#ifndef ESPRACK_BUILDER_H
#define ESPRACK_BUILDER_H

// Builder — composition root for ESPRack::App. The single .install<>()
// chain in the consumer's main.cpp expresses every framework feature
// the device wants. Builder validates the descriptor list, computes
// install order, then constructs App.
//
// Usage:
//
//   auto app = ESPRack::Builder(&server, "MyDevice", "v1.0.0")
//     .core()                                    // wifi+ap+security+...
//     .install<MqttModule>()
//     .install<LightControlModule>(/*pin=*/14)
//     .build();
//
// Methods chain. .build() returns an std::unique_ptr<App>.

#include <Arduino.h>
#include <memory>
#include <utility>
#include <vector>

#include "App.h"
#include "Module.h"

class AsyncWebServer;

namespace ESPRack {

class Builder {
 public:
  Builder(AsyncWebServer* server,
          const char* deviceName    = "ESPRackDevice",
          const char* deviceVersion = "v0.0.0");

  // Install a module. Constructed in-place from forwarded args.
  // Returns *this so calls chain.
  template <typename M, typename... Args>
  Builder& install(Args&&... args) {
    static_assert(std::is_base_of<Module, M>::value,
                  "ESPRack::Builder::install<M>() — M must derive from ESPRack::Module");
    modules_.emplace_back(std::unique_ptr<Module>(new M(std::forward<Args>(args)...)));
    return *this;
  }

  // ----- Preset bundles -------------------------------------------------
  //
  // Sugar over multiple .install<>() calls. Modules are added in the
  // order they conventionally need to come up. Consumer can call .core()
  // and then .install<>() additional modules; the final topological
  // sort in build() handles ordering correctly regardless.
  //
  // Implementations live in Builder.cpp because they #include each
  // module's header — keeping Builder.h dependency-light for consumers
  // that compose modules manually.

  // Default backbone: security + wifi + ap + system-status + filesystem
  // + ws-diag. Most projects want at least this.
  Builder& core();

  // .core() + ntp + ota + upload-firmware + auto-update.
  Builder& networking();

  // .networking() + mqtt + telegram + config-manager-ui + web-endpoints.
  Builder& full();

  // ----- Framework knobs ------------------------------------------------

  // Default ArduinoJson buffer size for modules that don't override.
  Builder& bufferSize(size_t bytes) { buffer_size_ = bytes; return *this; }

  // CORS origin appended to outgoing HTTP responses. Empty string disables.
  Builder& cors(const char* origin) { cors_ = origin ? origin : ""; return *this; }

  // WS keep-alive cadence — see WsManager::beginPingPong.
  Builder& wsKeepAlive(uint32_t ping_sec, uint32_t pong_ms) {
    ws_ping_sec_ = ping_sec;
    ws_pong_ms_ = pong_ms;
    return *this;
  }

  // ----- Build ----------------------------------------------------------
  //
  // Validate descriptors, topologically sort, transfer ownership to a
  // new App. The Builder is invalid after build() returns; do not
  // reuse it. On validation failure (missing dependency, cycle, dup id)
  // emits Serial diagnostics and aborts the boot — better fail-fast
  // than ship a half-installed framework.
  std::unique_ptr<App> build();

 private:
  AsyncWebServer*  server_ {nullptr};
  String           deviceName_;
  String           deviceVersion_;
  size_t           buffer_size_ {4096};
  String           cors_;
  uint32_t         ws_ping_sec_ {20};
  uint32_t         ws_pong_ms_  {60000};

  std::vector<std::unique_ptr<Module>> modules_;
};

}  // namespace ESPRack

#endif  // ESPRACK_BUILDER_H
