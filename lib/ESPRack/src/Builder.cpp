#include "Builder.h"
#include "App.h"
#include "hw/HwRegistry.h"

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
// Light log helper — wraps vsnprintf into a fixed buffer + emits via
// arduino-esp32's log_* macros (compile-stripped when CORE_DEBUG_LEVEL
// is below the level). Stays self-contained so it works in Phase 1
// before any other framework piece (incl. Serial) is fully wired.
inline void rerr(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char buf[160];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  log_e("[ESPRack] %s", buf);
}
inline void rinfo(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char buf[160];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  log_i("[ESPRack] %s", buf);
}
}  // namespace

std::unique_ptr<App> Builder::build() {
  // Step 1 — collect descriptors. An external HW instance with an invalid
  // id is dropped here (LIB-FEAT-001): reported, listed in System ->
  // Hardware, never installed.
  struct HwRejected {
    size_t idx;
    const char* why;
  };
  std::vector<HwRejected> hwRejected;
  std::vector<ModuleDescriptor> descs(modules_.size());
  for (size_t i = 0; i < modules_.size(); ++i) {
    if (!modules_[i]) continue;
    modules_[i]->describe(descs[i]);
    if (ExtHwModule* x = modules_[i]->asExtHw()) {
      if (!x->instanceIdValid()) {
        rerr("external HW instance '%s' (%s): invalid id, [a-z0-9_-] 1..%u chars; not installed",
             x->instanceId(), x->typeName(), (unsigned)ExtHwModule::kIdMax);
        hwRejected.push_back(HwRejected{i, "invalid instance id"});
        descs[i].id = nullptr;
        continue;
      }
      descs[i].id = x->instanceId();  // whatever describe() said: the id is the instance's
    }
    if (!descs[i].id || descs[i].id[0] == '\0') {
      rerr("module %zu has empty id", i);
    }
  }

  // Step 2 — index by id, detect duplicates. External HW instances come
  // second, so an instance id never displaces a framework module; a
  // duplicate instance is dropped like an invalid one.
  std::map<String, size_t> id_index;
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < descs.size(); ++i) {
      if (!descs[i].id) continue;
      const bool ext = modules_[i] && modules_[i]->asExtHw();
      if (ext != (pass == 1)) continue;
      String key{descs[i].id};
      if (id_index.count(key)) {
        rerr("duplicate module id '%s'", descs[i].id);
        if (ext) {
          hwRejected.push_back(HwRejected{i, "duplicate id"});
          descs[i].id = nullptr;
        }
      } else {
        id_index[key] = i;
      }
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
  ctx.server        = server_;
  ctx.cfgMgr        = app->configManager();
  ctx.web           = app->webManager();
  ctx.ws            = app->wsManager();
  ctx.security      = app->security();
  ctx.presence      = app->presence();
  ctx.app           = app.get();
  ctx.deviceName    = app->deviceName();
  ctx.deviceVersion = app->deviceVersion();

  // Hardware: consumer reservations before any module installs; rejected
  // instances are listed in System -> Hardware.
  for (const Reservation& r : reserved_) app->hw()->reserveGpio(r.pin, r.reason);
  for (const HwRejected& r : hwRejected) {
    app->hw()->addRejected(*modules_[r.idx]->asExtHw(), descs[r.idx].version, r.why);
  }

  // Reorder modules_ to match `order` then transfer to App.
  std::vector<std::unique_ptr<Module>> sorted;
  sorted.reserve(modules_.size());
  for (size_t idx : order) {
    if (modules_[idx]) {
      // Skip disabled modules (build-flag opt-out).
      if (!descs[idx].enabled) {
        rinfo("skipping disabled module '%s'", descs[idx].id);
        if (ExtHwModule* x = modules_[idx]->asExtHw()) {
          app->hw()->addRejected(*x, descs[idx].version, "disabled at build time");
        }
        continue;
      }
      // An external HW instance gets its registry (ExtHwModule::hw())
      // before onInstall.
      if (ExtHwModule* x = modules_[idx]->asExtHw()) app->hw()->addInstance(x);
      modules_[idx]->onInstall(ctx);
      sorted.push_back(std::move(modules_[idx]));
      rinfo("installed '%s' v%s priority=%d",
            descs[idx].id, descs[idx].version, descs[idx].priority);
      // Refresh late-bound pointers for any modules that come after —
      // security and presence may have been swapped in by the module
      // we just installed.
      ctx.security = app->security();
      ctx.presence = app->presence();
    }
  }
  app->adoptModules(std::move(sorted));
  return app;
}

}  // namespace ESPRack
