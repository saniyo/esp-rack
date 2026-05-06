#pragma once
#ifndef CONFIG_DELEGATE_H
#define CONFIG_DELEGATE_H

#include <Arduino.h>
#include <functional>

#include <ConfigManager.h>
#include <StatefulService.h>

template <typename T>
class ConfigDelegate {
 public:
  using Listener = std::function<void(const T& /*state*/, const String& /*origin*/)>;
  using ListenerId = ConfigManager::ListenerId;

  ConfigDelegate() = default;

  ConfigDelegate(ConfigManager* mgr,
                 const char* id,
                 const char* primary,
                 size_t bufferSize,
                 StatefulService<T>* service,
                 JsonStateReader<T> reader,
                 JsonStateUpdater<T> updater,
                 bool /*autoSave_unused*/ = false,
                 ConfigManager::ValidatorFn<T> validate = nullptr,
                 JsonStateReader<T> formReader = nullptr) {
    attach(mgr, id, primary, bufferSize, service, reader, updater, validate, formReader);
  }

  void attach(ConfigManager* mgr,
              const char* id,
              const char* primary,
              size_t bufferSize,
              StatefulService<T>* service,
              JsonStateReader<T> reader,
              JsonStateUpdater<T> updater,
              ConfigManager::ValidatorFn<T> validate = nullptr,
              JsonStateReader<T> formReader = nullptr) {
    _mgr = mgr;
    _entry = nullptr;
    _service = service;

    if (!_mgr || !id || !primary || !service) return;

    ConfigManager::ConfigDescriptor<T> d;
    d.id = id;
    d.primary = primary;
    d.bufferSize = bufferSize;
    d.service = service;
    d.reader = reader;
    d.updater = updater;
    d.validate = validate;
    d.formReader = formReader;
    // d.secretKeys is auto-populated inside registerConfig via the
    // form probe — never set here.

    _entry = _mgr->registerConfig<T>(d);
  }

  // ---- existing Binding<T> surface ---------------------------------------
  bool ensureLoaded() {
    if (!_mgr || !_entry) return false;
    return _mgr->ensureLoaded(_entry->id().c_str());
  }

  bool saveIfChanged(const String& origin = "manual") {
    if (!_mgr || !_entry) return false;
    return _mgr->saveIfChanged(_entry->id().c_str(), origin);
  }

  bool restoreFromBackup() {
    if (!_mgr || !_entry) return false;
    return _mgr->restoreFromBackup(_entry->id().c_str());
  }

  bool resetToDefaults() {
    if (!_mgr || !_entry) return false;
    return _mgr->resetToDefaults(_entry->id().c_str());
  }

  bool ok() const { return _mgr && _entry; }
  const char* id() const { return _entry ? _entry->id().c_str() : ""; }
  ConfigManager* manager() const { return _mgr; }
  StatefulService<T>* service() const { return _service; }

  // ---- NEW: typed observer -----------------------------------------------
  ListenerId subscribe(Listener cb) {
    if (!_entry || !cb || !_service) return 0;
    StatefulService<T>* svc = _service;
    ConfigManager::GenericListener generic =
        [svc, cb](ConfigManager::IConfigEntry&, const String& origin) {
          if (!svc) return;
          svc->read([&](T& state) { cb(state, origin); });
        };
    return _entry->addObserver(std::move(generic));
  }

  void unsubscribe(ListenerId id) {
    if (!_entry) return;
    _entry->removeObserver(id);
  }

  // ---- NEW: typed helpers -------------------------------------------------
  void read(std::function<void(const T&)> reader) const {
    if (!_service || !reader) return;
    _service->read([&](T& s) { reader(s); });
  }

  StateUpdateResult update(std::function<StateUpdateResult(T&)> mutator,
                           const String& origin = "manual") {
    if (!_service || !mutator) return StateUpdateResult::UNCHANGED;
    StateUpdateResult r = _service->update(mutator, origin);
    if (r == StateUpdateResult::CHANGED) {
      // persist + fire observers
      (void)saveIfChanged(origin);
    }
    return r;
  }

  T snapshot() const {
    T copy{};
    if (_service) _service->read([&](T& s) { copy = s; });
    return copy;
  }

 private:
  ConfigManager* _mgr{nullptr};
  ConfigManager::IConfigEntry* _entry{nullptr};
  StatefulService<T>* _service{nullptr};
};

#endif  // CONFIG_DELEGATE_H
