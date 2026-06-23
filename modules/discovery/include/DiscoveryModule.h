#pragma once
#ifndef ESPRACK_DISCOVERY_MODULE_H
#define ESPRACK_DISCOVERY_MODULE_H

// DiscoveryModule — LAN device discovery, built on the mDNS substrate.
//
// NOTE on naming: this is DISCOVERY (mDNS-based: each device advertises and
// browses for siblings on the LAN), NOT a mesh network. The name "mesh" is
// reserved for a future device-to-device mesh approach over WiFi. This module
// only finds peers and surfaces them on the Discovery page.
//
// The mDNS module is effectively part of this feature (it was created as the
// discovery substrate): DiscoveryModule depends on it, so installing Discovery
// pulls MdnsModule in, and they read as a single whole. Excluding this module —
// drop `DiscoveryModule` from the consumer's lib_deps + the
// `.install<DiscoveryModule>()` call — removes the entire feature (and its
// Discovery page) at compile time.
//
// Priority 24 puts us right after mdns (22): the responder must be up before
// we advertise our trust tag and browse for peers. We grab the resolver in
// onBegin (App::mdns() is late-bound by MdnsModule's onInstall).

#include <Module.h>
#include <App.h>
#include "DiscoveryService.h"
#include <memory>

class DiscoveryModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "discovery";
    d.version = "0.1.0";
    d.priority = 24;            // after mdns (22)
    d.requires_ = {"mdns"};
  }

  void onInstall(ESPRack::ModuleContext& ctx) override {
    app_ = ctx.app;
    svc_.reset(new DiscoveryService());
    svc_->registerManifest(ctx.web);   // Discovery page + /api/v1/discovery/peers
  }

  void onBegin() override {
    if (svc_ && app_) svc_->begin(app_->mdns());
  }

  DiscoveryService* service() { return svc_.get(); }

 private:
  std::unique_ptr<DiscoveryService> svc_;
  ESPRack::App* app_{nullptr};
};

#endif  // ESPRACK_DISCOVERY_MODULE_H
