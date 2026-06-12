#pragma once
#ifndef ESPRACK_MESH_MODULE_H
#define ESPRACK_MESH_MODULE_H

// MeshModule — LAN device-mesh discovery, built on the mDNS substrate.
//
// The mDNS module is effectively part of this feature (it was created as the
// discovery substrate): MeshModule depends on it, so installing Mesh pulls
// MdnsModule in, and they read as a single whole. Excluding this module —
// drop `MeshModule` from the consumer's lib_deps + the `.install<MeshModule>()`
// call — removes the entire feature (and its Devices page) at compile time.
//
// Priority 24 puts us right after mdns (22): the responder must be up before
// we advertise our trust tag and browse for peers. We grab the resolver in
// onBegin (App::mdns() is late-bound by MdnsModule's onInstall).

#include <Module.h>
#include <App.h>
#include "MeshService.h"
#include <memory>

class MeshModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "mesh";
    d.version = "0.1.0";
    d.priority = 24;            // after mdns (22)
    d.requires_ = {"mdns"};
  }

  void onInstall(ESPRack::ModuleContext& ctx) override {
    app_ = ctx.app;
    svc_.reset(new MeshService());
    svc_->registerManifest(ctx.web);   // Devices page + /api/v1/mesh/peers
  }

  void onBegin() override {
    if (svc_ && app_) svc_->begin(app_->mdns());
  }

  MeshService* service() { return svc_.get(); }

 private:
  std::unique_ptr<MeshService> svc_;
  ESPRack::App* app_{nullptr};
};

#endif  // ESPRACK_MESH_MODULE_H
