#pragma once
#ifndef ESPRACK_FEATURES_MODULE_H
#define ESPRACK_FEATURES_MODULE_H

// FeaturesModule — exposes the installed-feature set as /api/v1/features
// so the React frontend can adapt routing/menu to what's actually wired
// up. Module features are derived from the Builder().install<>() chain
// (via WebManager's install map); only a few non-module flags (FT_SECURITY,
// FT_PROJECT, …) still come from compile macros. Just wraps FeaturesService
// (which mounts its endpoint internally on construction).

#include <Module.h>
#include "FeaturesService.h"
#include <memory>

class FeaturesModule : public ESPRack::Module {
 public:
  void describe(ESPRack::ModuleDescriptor& d) override {
    d.id = "features"; d.version = "1.0.0"; d.priority = 5;
  }
  void onInstall(ESPRack::ModuleContext& ctx) override {
    // Pass WebManager so the service also registers a proxy endpoint
    // — without that, the mothership mship-ui reverse proxy returns
    // 404 for /api/v1/features and the React app's bootstrap fails.
    svc_.reset(new FeaturesService(ctx.server, ctx.web));
  }
 private:
  std::unique_ptr<FeaturesService> svc_;
};

#endif
