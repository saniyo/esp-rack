#pragma once
#ifndef ESPRACK_DISCOVERY_MODULE_H
#define ESPRACK_DISCOVERY_MODULE_H

// DiscoveryModule — LAN device discovery over a custom UDP membership protocol.
//
// NOTE on naming: this is DISCOVERY (each device announces itself and learns
// its siblings on the LAN), NOT a mesh network. The name "mesh" is reserved for
// a future device-to-device mesh approach over WiFi. This module only finds
// peers and surfaces them on the Discovery page.
//
// Transport: a fixed UDP socket (multicast self-announce for bootstrap +
// unicast/gossip membership), NOT mDNS. The mDNS responder was removed — it
// cost ~12-15 KB of heap on the no-PSRAM C3 and only browse/advertise was used.
// DiscoveryService now owns its own socket and needs nothing from the App.
// Excluding this module — drop `DiscoveryModule` from the consumer's lib_deps +
// the `.install<DiscoveryModule>()` call — removes the entire feature (and its
// Discovery page) at compile time.
//
// Priority 24 puts us after wifi (10): STA must have an IP before we open the
// socket / join the multicast group. begin() is self-contained.

#include <Module.h>
#include <App.h>
#include "DiscoveryService.h"
#include <memory>

class DiscoveryModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "discovery";
    d.version = "0.1.0";
    d.priority = 24;            // after wifi (10)
    d.requires_ = {"wifi"};
  }

  void onInstall(ESPRack::ModuleContext& ctx) override {
    svc_.reset(new DiscoveryService());
    svc_->registerManifest(ctx.web);   // Discovery page + /api/v1/discovery/peers
  }

  void onBegin() override {
    if (svc_) svc_->begin();
  }

  DiscoveryService* service() { return svc_.get(); }

 private:
  std::unique_ptr<DiscoveryService> svc_;
};

#endif  // ESPRACK_DISCOVERY_MODULE_H
