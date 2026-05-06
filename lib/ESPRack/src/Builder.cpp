#include "Builder.h"
#include "App.h"

#include <algorithm>
#include <map>
#include <set>

namespace ESPRack {

Builder::Builder(AsyncWebServer* server,
                 const char* deviceName,
                 const char* deviceVersion) :
    server_       {server},
    deviceName_   {deviceName ? deviceName : "ESPRackDevice"},
    deviceVersion_{deviceVersion ? deviceVersion : "v0.0.0"} {}

// ----- Preset bundles -----------------------------------------------------
//
// Forward declarations of the built-in modules' types live in their
// own headers (modules/<name>/include/<Name>Module.h). The presets
// below are intentionally LEFT EMPTY in this Phase 1.0 commit — the
// modules don't exist yet. They'll be filled in incrementally as each
// module lands. Consumers wanting just WiFi can call:
//
//   .install<WiFiModule>()
//
// directly without any preset.
Builder& Builder::core() {
  // TODO: install<SecurityModule>() (priority 5)
  // TODO: install<WiFiModule>()     (priority 10)
  // TODO: install<APModule>()       (priority 10)
  // TODO: install<FileSystemModule>()
  // TODO: install<SystemStatusModule>()
  // TODO: install<WsDiagModule>()
  return *this;
}
Builder& Builder::networking() { return core(); /* TODO add ntp/ota/auto-update */ }
Builder& Builder::full()       { return networking(); /* TODO add mqtt/telegram/etc. */ }

// ----- Build --------------------------------------------------------------

namespace {
// Light log helper. We avoid pulling a logging dependency in Phase 1.
inline void rerr(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  Serial.print(F("[ESPRack] ERROR: "));
  char buf[160];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
}
inline void rinfo(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  Serial.print(F("[ESPRack] "));
  char buf[160];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
}
}  // namespace

std::unique_ptr<App> Builder::build() {
  // Step 1 — collect descriptors.
  std::vector<ModuleDescriptor> descs(modules_.size());
  for (size_t i = 0; i < modules_.size(); ++i) {
    if (!modules_[i]) continue;
    modules_[i]->describe(descs[i]);
    if (!descs[i].id || descs[i].id[0] == '\0') {
      rerr("module %zu has empty id", i);
    }
  }

  // Step 2 — index by id, detect duplicates.
  std::map<String, size_t> id_index;
  for (size_t i = 0; i < descs.size(); ++i) {
    if (!descs[i].id) continue;
    String key{descs[i].id};
    if (id_index.count(key)) {
      rerr("duplicate module id '%s'", descs[i].id);
    } else {
      id_index[key] = i;
    }
  }

  // Step 3 — verify every dependency exists.
  for (const auto& d : descs) {
    for (const char* req : d.requires_) {
      if (!id_index.count(String{req})) {
        rerr("module '%s' requires '%s' but it is not installed", d.id, req);
      }
    }
  }

  // Step 4 — Kahn's algorithm topological sort. Within a level, sort by
  // (priority asc, id asc) for deterministic order.
  std::map<String, std::vector<String>> deps_of;     // id → list of deps
  std::map<String, int>                  in_degree;  // id → number of unmet deps
  std::map<String, std::vector<String>> dependents; // id → who depends on me
  for (const auto& d : descs) {
    if (!d.id) continue;
    String key{d.id};
    in_degree[key] = (int)d.requires_.size();
    for (const char* r : d.requires_) {
      dependents[String{r}].push_back(key);
      deps_of[key].push_back(String{r});
    }
  }

  auto cmp = [&](const String& a, const String& b) {
    int pa = descs[id_index[a]].priority;
    int pb = descs[id_index[b]].priority;
    if (pa != pb) return pa < pb;
    return a < b;
  };

  std::vector<String> ready;
  for (auto& kv : in_degree) {
    if (kv.second == 0) ready.push_back(kv.first);
  }

  std::vector<size_t> order;
  while (!ready.empty()) {
    std::sort(ready.begin(), ready.end(), cmp);
    String next = ready.front();
    ready.erase(ready.begin());
    order.push_back(id_index[next]);
    for (const String& dep : dependents[next]) {
      if (--in_degree[dep] == 0) ready.push_back(dep);
    }
  }

  if (order.size() != id_index.size()) {
    rerr("dependency cycle detected — refusing to build");
    // Best-effort: fall back to insertion order so something boots.
    order.clear();
    for (size_t i = 0; i < modules_.size(); ++i) {
      if (descs[i].id) order.push_back(i);
    }
  }

  // Step 5 — instantiate App, run onInstall in topological order.
  auto app = std::unique_ptr<App>(new App(server_, deviceName_.c_str(), deviceVersion_.c_str()));

  ModuleContext ctx;
  ctx.server   = server_;
  ctx.cfgMgr   = app->configManager();
  ctx.web      = app->webManager();
  ctx.ws       = app->wsManager();
  ctx.security = app->security();

  // Reorder modules_ to match `order` then transfer to App.
  std::vector<std::unique_ptr<Module>> sorted;
  sorted.reserve(modules_.size());
  for (size_t idx : order) {
    if (modules_[idx]) {
      // Skip disabled modules (build-flag opt-out).
      if (!descs[idx].enabled) {
        rinfo("skipping disabled module '%s'", descs[idx].id);
        continue;
      }
      modules_[idx]->onInstall(ctx);
      sorted.push_back(std::move(modules_[idx]));
      rinfo("installed '%s' v%s priority=%d",
            descs[idx].id, descs[idx].version, descs[idx].priority);
      // Modules may have replaced the SecurityManager during onInstall;
      // refresh the context for any modules that come after.
      ctx.security = app->security();
    }
  }
  app->adoptModules(std::move(sorted));
  return app;
}

}  // namespace ESPRack
