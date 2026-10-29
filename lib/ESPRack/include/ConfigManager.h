#pragma once
#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <FS.h>
#include <ArduinoJson.h>
#include "power/FlashWriter.h"
#include "power/PowerGuard.h"
#include "power/WriteGate.h"

#include <atomic>
#include <vector>
#include <memory>
#include <functional>

#include <StatefulService.h>
#include <SecretsVault.h>

#ifndef CFGMGR_USE_FREERTOS
#define CFGMGR_USE_FREERTOS 1
#endif

// forward for FreeRTOS mutex
#if CFGMGR_USE_FREERTOS
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

// How long a caller on a shared task (async_tcp, the loop: Save Backup, the
// restart's restore) waits for the config lock before its error path.
#ifndef CFGMGR_LOCK_WAIT_MS
#define CFGMGR_LOCK_WAIT_MS 1000
#endif
// A deferred save whose service state lock timed out (StatefulService):
// the FlashWriter tries it again after this long.
#ifndef CFGMGR_BUSY_RETRY_MS
#define CFGMGR_BUSY_RETRY_MS 1000
#endif

class ConfigManager {
 public:
  struct VerifyInfo {
    bool ok{false};
    bool legacy{false};
    bool hadMeta{false};
    bool crcChecked{false};
  };

  enum class LoadResult {
    OkPrimary,
    OkRestoredFromBackup,
    // The verified backup's data, the primary not repaired (the write gate
    // or PowerGuard turned the copy away, or it failed): the next save
    // writes the primary without rotating it over the backup.
    OkBackupOnly,
    NeedDefaults,
    Fail
  };

  // (на майбутнє) — тут можна буде накопичувати статистику
  struct EntryStats {
    uint32_t loads{0};
    uint32_t saves{0};
    uint32_t skipped{0};
    uint32_t deferred{0};          // saves PowerGuard paused
    uint32_t restores{0};
    uint32_t resets{0};
    uint32_t lastCrc{0};
    uint32_t lastTs{0};            // wall-time secs of last successful save
    uint32_t snaps{0};             // count of successful snapshot rotations
                                   // (auto on save AND manual Save Backup)
    uint32_t lastSnapshotTs{0};    // wall-time secs of last snapshot write —
                                   // drives the Config Manager tab's "Age (s)"
                                   // column; reflects the snapshot file's
                                   // freshness, not the live primary's
  };

  explicit ConfigManager(fs::FS* fs = nullptr);

  void setFS(fs::FS* fs) { _fs = fs; }
  fs::FS* fs() const { return _fs; }

  // викликати ПІСЛЯ ESPFS.begin(...)
  // створює /config та вибирає робочі tmp/bak директорії:
  //  1) /config/.tmp & /config/.bak
  //  2) якщо не можна — /config/_tmp & /config/_bak
  void begin();

  // ---- descriptor ----
  template <typename T>
  using ValidatorFn = std::function<bool(JsonObjectConst)>;

  template <typename T>
  struct ConfigDescriptor {
    const char* id{nullptr};          // "lightState"
    const char* primary{nullptr};     // "/config/lightState.json"
    const char* backup{nullptr};      // optional override (інакше менеджер сам)
    const char* tmp{nullptr};         // optional override (інакше менеджер сам)
    size_t bufferSize{4096};

    StatefulService<T>* service{nullptr};
    JsonStateReader<T> reader;
    JsonStateUpdater<T> updater;

    // autoSave removed — save is always explicit via saveIfChanged()
    ValidatorFn<T> validate{nullptr};

    // Optional buildForm pointer. When non-null, registerConfig probes
    // it once with a default-constructed state, walks the resulting
    // form schema and auto-populates `secretKeys` from every leaf whose
    // option-string starts with "secret;" (FieldType::SECRET marker).
    // Services use FormBuilder::addSecretField for password / token /
    // signing-key fields and never touch secretKeys manually.
    JsonStateReader<T> formReader{nullptr};

    // Top-level JSON keys that must be encrypted at rest. Auto-populated
    // from formReader at registration; not meant to be set by callers
    // any more (left in the public surface so the encrypt/decrypt walk
    // implementation in ConfigManager stays straightforward).
    std::vector<String> secretKeys{};
  };

  // ---- observer types (generic, type-erased) ----
  using ListenerId = uint32_t;
  class IConfigEntry;  // fwd
  using GenericListener = std::function<void(IConfigEntry&, const String& /*origin*/)>;

  // ---- interface for entries (type-erased) ----
  class IConfigEntry {
   public:
    virtual ~IConfigEntry() = default;
    virtual const String& id() const = 0;
    virtual const String& primary() const = 0;
    virtual bool ensureLoaded(ConfigManager& mgr) = 0;
    virtual bool saveIfChanged(ConfigManager& mgr, const String& origin) = 0;
    virtual bool restoreFromBackup(ConfigManager& mgr) = 0;
    virtual bool resetToDefaults(ConfigManager& mgr) = 0;
    virtual EntryStats& stats() = 0;
    virtual const EntryStats& stats() const = 0;
    // A save the write gate put off; the FlashWriter job writes it.
    // deferredSince: when it was first put off (WriteGate::now()).
    virtual bool deferred() const { return false; }
    virtual uint32_t deferredSince() const { return 0; }
    // Loaded from the backup while the primary on flash failed to verify
    // and is not rewritten yet: never copy that primary over the backup.
    virtual bool primaryStale() const { return false; }
    // FlashWriter, the first step of a deferred save: the backup copy of
    // the file on flash. Unneeded: nothing to save any more (the state is
    // the saved one again; no longer deferred). Paused: PowerGuard paused
    // writes, no copy made (it stays deferred). Busy: the service's state
    // lock timed out, no copy made (it stays deferred, tried again soon).
    enum class Backup : uint8_t { Unneeded, Made, Paused, Busy };
    virtual Backup backupForSave(ConfigManager&) { return Backup::Made; }
    // A save whose caller holds a gate slot (slotHeld: no gate here) and
    // has made the backup copy already (backupDone). saveIfChanged() is
    // saveStep(mgr, origin, false, false).
    virtual bool saveStep(ConfigManager& mgr, const String& origin, bool slotHeld, bool backupDone) {
      (void)slotHeld;
      (void)backupDone;
      return saveIfChanged(mgr, origin);
    }

    // observer API — fires AFTER successful atomicWrite on CHANGED only
    virtual ListenerId addObserver(GenericListener l) = 0;
    virtual void removeObserver(ListenerId id) = 0;
  };

  // ---- broker/binding: красиво в initializer list ----
  template <typename T>
  class Binding {
   public:
    Binding() = default;

    Binding(ConfigManager* mgr,
            const char* id,
            const char* primary,
            size_t bufferSize,
            StatefulService<T>* service,
            JsonStateReader<T> reader,
            JsonStateUpdater<T> updater,
            bool /*autoSave_unused*/ = false,
            ValidatorFn<T> validate = nullptr) {
      attach(mgr, id, primary, bufferSize, service, reader, updater, validate);
    }

    void attach(ConfigManager* mgr,
                const char* id,
                const char* primary,
                size_t bufferSize,
                StatefulService<T>* service,
                JsonStateReader<T> reader,
                JsonStateUpdater<T> updater,
                ValidatorFn<T> validate = nullptr) {
      _mgr = mgr;
      _entry = nullptr;

      if (!_mgr || !id || !primary || !service) return;

      ConfigDescriptor<T> d;
      d.id = id;
      d.primary = primary;
      d.bufferSize = bufferSize;
      d.service = service;
      d.reader = reader;
      d.updater = updater;
      d.validate = validate;

      _entry = _mgr->registerConfig<T>(d);
    }

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

   private:
    ConfigManager* _mgr{nullptr};
    IConfigEntry* _entry{nullptr};
  };

  // ---- manager API ----
  template <typename T>
  IConfigEntry* registerConfig(ConfigDescriptor<T> desc) {
    if (!desc.id || !desc.primary || !desc.service) return nullptr;

    // Auto-discover SECRET-typed fields from the form schema so the
    // service author never maintains a `secretKeys` list manually.
    discoverSecretKeysFromForm<T>(desc);

    lock();
    if (findEntry(desc.id)) {
      unlock();
      return findEntry(desc.id);
    }

    // C++11 friendly: без make_unique
    std::unique_ptr<ConfigEntry<T>> ptr(new ConfigEntry<T>(desc));
    ptr->bindManager(this);
    IConfigEntry* raw = ptr.get();
    _entries.emplace_back(std::move(ptr));
    unlock();
    return raw;
  }

  // Auto-discovery — calls the service's buildForm against a default
  // state, walks the produced JSON tree, collects every leaf whose
  // option string starts with "secret;" into desc.secretKeys.
  // Implemented inline because it needs T at compile time. The walk
  // itself is type-erased (operates on JsonObject) so the only
  // template work here is constructing the scratch state + invoking
  // the typed reader.
  template <typename T>
  void discoverSecretKeysFromForm(ConfigDescriptor<T>& desc);

  bool ensureLoaded(const char* id);
  bool saveIfChanged(const char* id, const String& origin = "");

  // Saves the write gate (power/WriteGate.h) put off: PowerGuard paused
  // writes, or a module holds windows. A save runs at once (any task) only
  // while the gate is free; otherwise it is marked deferred and the
  // FlashWriter task writes it, in two steps of one slot each (the backup
  // copy, then the file), no polling. A PowerGuard latch keeps them until
  // restart. flushDeferred() kicks that job. flushDeferredNow() (the
  // restart path) waits for the gate at most boundMs per save, then writes
  // anyway: a save left waiting would be lost with the restart. A queued
  // Save Backup (requestSnapshot) follows the saves, one wait for its run.
  void markDeferred() {
    _anyDeferred = true;
    ESPRack::FlashWriter::kick(_writerJob);
  }
  // An entry's state lock timed out: its save stays deferred and the
  // FlashWriter tries it again in CFGMGR_BUSY_RETRY_MS (not on a power
  // event: nothing paused the writes).
  void noteBusy() { _busyRetry.store(true); }
  bool hasDeferred() const { return _anyDeferred.load(); }
  size_t deferredCount() const;
  void flushDeferred();
  void flushDeferredNow(uint32_t boundMs);
  // The previous file to <backup> (best effort): atomicWrite's first step.
  void backupPrimary(const char* primary, const char* backup);
  bool restoreFromBackup(const char* id);
  bool resetToDefaults(const char* id);

  EntryStats* statsOf(const char* id);

  // ---- public iteration over registered entries ----
  // Used by ConfigManagerService to render the per-entry table. NOT
  // locked — the entry vector is only mutated during early init
  // (registerConfig from service ctors) which runs single-threaded
  // before WebManager::begin(). Callers MUST NOT trigger save /
  // restore / reset on the entry from inside the callback (that path
  // takes the manager's mutex which we already hold by convention
  // during config-manipulation flows; reentry would deadlock on the
  // non-recursive FreeRTOS mutex).
  template <typename Fn>
  void forEachEntry(Fn cb) {
    for (auto& e : _entries) if (e) cb(*e);
  }

  // ---- bulk dump / restore (Phase 6 — mothership-driven backup) ----
  //
  // dumpAll walks every registered entry, reads its primary file
  // verbatim (encrypted secrets travel as the same ENC: blobs they
  // live as on disk — backup payloads can't leak credentials), and
  // emits them under their entry id key:
  //
  //   { "lightState": { "data": {...}, "meta": {...} },
  //     "wifi":       { "data": {...}, "meta": {...} },
  //     ... }
  //
  // Returns false if a single read errors; partial dumps are not
  // emitted — caller can retry.
  //
  // restoreAll is the reverse: walk each entry, fetch the matching
  // sub-JSON from `in`, and overwrite the entry's primary file
  // atomically. Skips entries absent from `in` (so a partial backup
  // restore only touches what it carries). Returns the count of
  // entries actually written. Caller should reboot after a non-zero
  // count so each service's begin() picks up the restored data
  // cleanly — restoreAll deliberately does NOT call ensureLoaded
  // on each entry because the live state would diverge from disk
  // until next reboot anyway.
  bool dumpAll(JsonObject out);
  size_t restoreAll(JsonObjectConst in);

  // ---- snapshot operations ----
  // Each primary file foo.json has a sibling foo.json.snapshot used as both
  // (a) the auto-rotated previous-primary recovery file (atomicWrite copies
  //     primary → snapshot before opening primary for write), and
  // (b) the operator-frozen "save current state" point — the new Config
  //     Manager tab forces snapshot = primary across every registered
  //     entry in one gesture via snapshotAll().
  // hasSnapshot tells the UI whether a row's `Snap` column should show ✓.
  // restoreAllFromSnapshot() is non-destructive to the snapshot itself —
  // it copies snapshot → primary and leaves snapshot in place; caller
  // (action handler) reboots so each service's begin() picks up the
  // restored primary cleanly. It runs under the lock (at most
  // CFGMGR_LOCK_WAIT_MS to get it) and the write gate: the restart's
  // after-shutdown step holds a slot for it (GateHeld).
  bool snapshotEntry(const char* primaryPath);
  bool restoreEntryFromSnapshot(const char* primaryPath);
  bool hasSnapshot(const char* primaryPath) const;
  // Now, any task, never waits for the gate: under the lock (at most
  // CFGMGR_LOCK_WAIT_MS) with one write gate pass for the run, only while
  // the gate is free. false: a copy failed, or the lock / gate said no.
  bool snapshotAll();
  bool restoreAllFromSnapshot();

  // Save Backup from a shared task (async_tcp), never waits: snapshotAll()
  // now when it can (a free gate: as without the gate); otherwise queued
  // for the FlashWriter job, a gate slot per file step, after the deferred
  // saves (so it freezes what they write). PowerGuard pausing writes keeps
  // it queued; the restart path (flushDeferredNow) writes it at the latest.
  enum class SnapState : uint8_t { None, Queued, Done, Failed };
  SnapState requestSnapshot();
  SnapState snapshotState() const;  // the last request's outcome so far
  uint32_t snapshotStateTs() const;  // when it got there (nowTs() secs)

  // ---- the write gate for the file helpers ----
  // copyFile / removeIfExists / mkdirsForFile each write one bounded step;
  // atomicWrite and restoreBackupToPrimary a few (they go only while the
  // gate is free). A helper takes a write gate pass (power/WriteGate.h) of
  // its own for that, never waits, and fails like a flash error when the
  // gate says no (the caller keeps what it has). A task that holds a slot
  // or pass for its config work already (a save, the FlashWriter job, the
  // restart's after-shutdown step) says so with a GateHeld for that scope:
  // the helpers then take none (no second entry into the gate).
  class GateHeld {
   public:
    explicit GateHeld(bool on = true);
    ~GateHeld();
    static bool here();  // the calling task holds one
    GateHeld(const GateHeld&) = delete;
    GateHeld& operator=(const GateHeld&) = delete;

   private:
    bool _on = false;
  };

  // ---- internal helpers used by entries/templates ----
  void lock();
  void unlock();
  bool tryLock(uint32_t ms);  // false: not within ms (the caller's error path)

  String makeBackupPathFor(const char* primary) const;
  String makeTmpPathFor(const char* primary) const;

  bool fileExists(const char* path) const;
  bool removeIfExists(const char* path);
  bool mkdirsForFile(const char* filePath);
  bool copyFile(const char* src, const char* dst);

  bool readJsonFile(const char* path, DynamicJsonDocument& doc);

  static uint32_t crc32OfJson(JsonObjectConst data);
  static String crc32ToHex(uint32_t crc);
  static bool hexToCrc32(const char* hex, uint32_t& out);

  // Walk `secretKeys` and rewrite each top-level string value through
  // SecretsVault::encrypt / decrypt. Both helpers are no-op-safe for
  // missing keys, empty values, and idempotent over already-prefixed
  // ciphertext (encrypt skips ENC: passthrough strings, decrypt
  // passes plaintext through unchanged). Returns true if at least one
  // key was found in plaintext form during a decrypt pass — caller
  // uses that to trigger a one-shot migration save so the file is
  // re-written in encrypted form on the next tick.
  static bool encryptSecretsInPlace(JsonObject root, const std::vector<String>& keys);
  static bool decryptSecretsInPlace(JsonObject root, const std::vector<String>& keys);

  // Probe-mode signal: returns true while ConfigManager is calling a
  // service's buildForm during registration to discover SECRET fields.
  // Future buildForms whose Status-tab content reads runtime peripherals
  // (uninitialised at registration time) can guard with
  // `if (!ConfigManager::isProbing()) { ... runtime fields ... }`. The
  // SECRET-typed fields themselves should ALWAYS be emitted because
  // their structure is what the probe is hunting for.
  static bool isProbing();

  static bool extractData(JsonVariantConst root, JsonObjectConst& dataOut, bool& legacyOut);
  bool verifyDoc(const DynamicJsonDocument& doc, VerifyInfo& info);
  bool readAndVerify(const char* path, DynamicJsonDocument& doc, VerifyInfo& info);

  bool writeRootWithMeta(File& f, JsonObjectConst data, uint32_t crc, uint32_t ts);

  bool restoreBackupToPrimary(const char* primary, const char* backup, const char* tmp);

  LoadResult loadOrRecover(const char* primary,
                           const char* backup,
                           const char* tmp,
                           size_t bufferSize,
                           DynamicJsonDocument& outDoc,
                           VerifyInfo* outPrimary,
                           VerifyInfo* outBackup);

  bool atomicWrite(const char* primary,
                   const char* backup,
                   const char* tmp,
                   JsonObjectConst data,
                   size_t bufferSize,
                   uint32_t* outCrc);

  uint32_t nowTs() const;

 private:
  template <typename T>
  class ConfigEntry : public IConfigEntry {
   public:
    explicit ConfigEntry(const ConfigDescriptor<T>& d) : _d(d), _id(d.id ? d.id : ""), _primary(d.primary ? d.primary : "") {}

    const String& id() const override { return _id; }
    const String& primary() const override { return _primary; }
    EntryStats& stats() override { return _stats; }
    const EntryStats& stats() const override { return _stats; }
    bool deferred() const override { return _deferred; }
    uint32_t deferredSince() const override { return _deferredSince; }
    bool primaryStale() const override { return _primaryStale; }
    bool _deferred = false;
    uint32_t _deferredSince = 0;
    bool _primaryStale = false;  // under the manager's lock

    Backup backupForSave(ConfigManager& mgr) override {
      if (!_d.service) return Backup::Unneeded;
      ensurePaths(mgr);
      mgr.lock();
      if (!ESPRack::PowerGuard::writesAllowed()) {
        mgr.unlock();
        return Backup::Paused;
      }
      DynamicJsonDocument dataDoc(_d.bufferSize);
      JsonObject data = dataDoc.to<JsonObject>();
      if (!_d.service->read(data, _d.reader)) {
        mgr.unlock();
        mgr.noteBusy();
        return Backup::Busy;  // state lock timed out: nothing read, no copy
      }
      if (_hasLastSavedCrc && ConfigManager::crc32OfJson(data) == _lastSavedCrc) {
        _deferred = false;  // back to what is on flash: nothing to write
        mgr.unlock();
        return Backup::Unneeded;
      }
      // A primary that failed to verify is no backup: the good one stays.
      if (!_primaryStale) mgr.backupPrimary(_primary.c_str(), _backup.c_str());
      mgr.unlock();
      return Backup::Made;
    }
    bool ensureLoaded(ConfigManager& mgr) override {
      if (_loaded) return true;
      if (!_d.service) return false;

      // підготовка шляхів (один раз)
      ensurePaths(mgr);

      mgr.lock();

      DynamicJsonDocument doc(_d.bufferSize);
      VerifyInfo vP, vB;

      auto res = mgr.loadOrRecover(_primary.c_str(), _backup.c_str(), _tmp.c_str(), _d.bufferSize, doc, &vP, &vB);

      if (res == LoadResult::NeedDefaults) {
        // Seed defaults into RAM via updater(empty).
        DynamicJsonDocument emptyDoc(_d.bufferSize);
        JsonObject empty = emptyDoc.to<JsonObject>();
        _d.service->updateWithoutPropagation(empty, _d.updater);
        _loaded = true;

        // RELIABILITY GUARD: persist defaults ONLY on a genuine first boot —
        // when NEITHER the primary NOR the snapshot file exists on disk. If a
        // file IS present but failed to load/verify (transient FS error, CRC
        // drift across a reflash, partial corruption), do NOT overwrite it
        // with defaults: keep it on disk and run on defaults in RAM for this
        // boot, so a later successful boot recovers the real config.
        // Saving defaults here would rotate the user's real config into
        // <primary>.snapshot and then never auto-restore it (the fresh
        // defaults primary verifies fine on the next boot), silently
        // destroying settings on a single spurious load failure — exactly the
        // "config wiped on reflash" symptom this guard exists to prevent.
        const bool haveFileOnDisk =
            mgr.fileExists(_primary.c_str()) || mgr.fileExists(_backup.c_str());

        mgr.unlock();
        _stats.loads++;

        if (haveFileOnDisk) {
          log_w("[CFG] %s: load FAILED but a config/snapshot file exists — "
                "running defaults in RAM, NOT overwriting (will retry next "
                "boot). Persisted settings preserved.",
                _primary.c_str());
        } else {
          _hasLastSavedCrc = false;  // genuine first boot → seed the file
          (void)saveIfChanged(mgr, "defaults");
        }
        return true;
      }

      if (res == LoadResult::OkPrimary || res == LoadResult::OkRestoredFromBackup ||
          res == LoadResult::OkBackupOnly) {
        // The primary failed to verify and is not repaired yet: whatever
        // is saved next (normalize, reset) is not rotated over the backup.
        _primaryStale = (res == LoadResult::OkBackupOnly);
        JsonObjectConst root = doc.as<JsonObjectConst>();
        JsonObjectConst dataConst;
        bool legacy = false;

        if (!ConfigManager::extractData(root, dataConst, legacy)) {
          mgr.unlock();
          return resetToDefaults(mgr);
        }

        if (_d.validate && !_d.validate(dataConst)) {
          mgr.unlock();
          return resetToDefaults(mgr);
        }

        // apply data
        DynamicJsonDocument inDoc(_d.bufferSize);
        JsonObject in = inDoc.to<JsonObject>();
        in.set(dataConst);

        // Decrypt secrets IN PLACE before the updater sees the JSON (also
        // any top-level ENC: record left by the probe's fail-safe). Returns
        // true when the file needs a normalize-save: a secret was plaintext
        // on disk, or a non-secret was stored encrypted.
        bool foundPlaintext = ConfigManager::decryptSecretsInPlace(in, _d.secretKeys);

        _d.service->updateWithoutPropagation(in, _d.updater);

        cacheCrcFromReader(mgr);

        bool needNormalize = false;
        if (legacy) needNormalize = true;
        if (!legacy && !vP.hadMeta) needNormalize = true;
        if (res == LoadResult::OkRestoredFromBackup) needNormalize = true;
        // The normalize save repairs a stale primary (deferred while the
        // write gate is busy).
        if (_primaryStale) needNormalize = true;
        if (foundPlaintext) needNormalize = true;

        _loaded = true;


        mgr.unlock();

        _stats.loads++;

        if (needNormalize) {
          _hasLastSavedCrc = false;  // force normalize write
          (void)saveIfChanged(mgr, "normalize");
        }
        return true;
      }

      mgr.unlock();
      return false;
    }

    bool saveIfChanged(ConfigManager& mgr, const String& origin) override {
      return saveStep(mgr, origin, false, false);
    }

    bool saveStep(ConfigManager& mgr, const String& origin, bool slotHeld, bool backupDone) override {
      if (!_d.service) return false;

      ensurePaths(mgr);

      mgr.lock();

      DynamicJsonDocument dataDoc(_d.bufferSize);
      JsonObject data = dataDoc.to<JsonObject>();
      // The state lock timed out: `data` is empty, never write it as the
      // config. The save stays deferred; the FlashWriter tries it again.
      if (!_d.service->read(data, _d.reader)) {
        if (!_deferred) _deferredSince = ESPRack::WriteGate::now();
        _deferred = true;
        mgr.unlock();
        mgr.noteBusy();
        if (!slotHeld) mgr.markDeferred();  // the writer's own run reports it itself
        return false;
      }

      if (_d.validate && !_d.validate(data)) {
        mgr.unlock();
        return false;
      }

      // Skip-check CRC is computed over PLAINTEXT (state-level),
      // before encryption rewrites the secret fields with random IVs.
      // _lastSavedCrc tracks the same plaintext CRC, so an unchanged
      // state always skips the flash write even though encryption
      // would otherwise produce a fresh ciphertext on every call.
      uint32_t crcPlain = ConfigManager::crc32OfJson(data);

      if (_hasLastSavedCrc && crcPlain == _lastSavedCrc) {
        _stats.skipped++;
        _deferred = false;
        mgr.unlock();
        return true;  // skip flash write
      }

      // The flash write gate (power/WriteGate.h). Here (async_tcp, the
      // loop, any task) the save goes only while the gate is free: it is
      // two files, more than a pause between a module's windows takes.
      // Otherwise the FlashWriter task writes it (markDeferred), a step per
      // pause; the RAM state is in effect already.
      const bool held = slotHeld;
      if (!_deferred) _deferredSince = ESPRack::WriteGate::now();
      struct GatePass {
        bool on;
        uint32_t at;
        void release() {
          if (on) ESPRack::WriteGate::leave(at);
          on = false;
        }
        ~GatePass() { release(); }
      } gate{false, 0};
      if (!held) gate.on = ESPRack::WriteGate::tryEnter(_deferredSince, true, &gate.at);
      if (!held && !gate.on) {
        if (!_deferred) {
          _stats.deferred++;
          ESPRack::WriteGate::noteDeferred();
        }
        _deferred = true;
        mgr.unlock();
        mgr.markDeferred();
        return false;
      }

      // Encrypt secret fields IN PLACE just before atomicWrite — the
      // file on disk gets the ENC:<hex> form, but the in-memory state
      // (and any subsequent reader() call) stays plaintext. A ciphertext
      // that doesn't fit the buffer would leave a null in the document:
      // never write that (the previous file stays).
      if (!ConfigManager::encryptSecretsInPlace(data, _d.secretKeys)) {
        mgr.unlock();
        return false;
      }

      // Was a primary file already on disk before this save? If so,
      // atomicWrite() rotates it into <primary>.snapshot, which counts
      // as a snapshot write — surface that on the Config Manager tab
      // by bumping snaps + lastSnapshotTs. First-save-ever (no primary
      // yet) skips the snapshot bookkeeping because no file rotated.
      // A stale primary (failed to verify) is not rotated: the backup stays.
      bool primaryExisted = !_primaryStale && mgr.fileExists(_primary.c_str());

      uint32_t written = 0;
      bool ok;
      {
        ConfigManager::GateHeld mark;  // the pass or the caller's slot covers atomicWrite's steps
        ok = mgr.atomicWrite(_primary.c_str(), (backupDone || _primaryStale) ? nullptr : _backup.c_str(),
                             _tmp.c_str(), data, _d.bufferSize, &written);
      }
      gate.release();

      std::vector<std::pair<ListenerId, GenericListener>> snap;
      if (ok) {
        // Track plaintext CRC for skip detection. The on-disk CRC
        // (`written`) goes into stats.lastCrc for diagnostic display.
        _lastSavedCrc = crcPlain;
        _hasLastSavedCrc = true;
        _deferred = false;
        _primaryStale = false;

        uint32_t now = mgr.nowTs();
        _stats.saves++;
        _stats.lastCrc = written;
        _stats.lastTs  = now;
        if (primaryExisted) {
          _stats.snaps++;
          _stats.lastSnapshotTs = now;
        }

        snap = _observers;  // snapshot under lock
      }

      mgr.unlock();

      // fire observers outside the lock; only when CHANGED (atomicWrite succeeded)
      if (ok && !snap.empty()) {
        for (auto& p : snap) {
          try {
            if (p.second) p.second(*this, origin);
          } catch (...) {
            // swallow — one broken listener must not kill others
          }
        }
      }
      return ok;
    }

    ListenerId addObserver(GenericListener l) override {
      if (!l) return 0;
      ListenerId id = ++_nextObserverId;
      if (_mgr) _mgr->lock();
      _observers.emplace_back(id, std::move(l));
      if (_mgr) _mgr->unlock();
      return id;
    }

    void removeObserver(ListenerId id) override {
      if (!id) return;
      if (_mgr) _mgr->lock();
      for (auto it = _observers.begin(); it != _observers.end(); ++it) {
        if (it->first == id) { _observers.erase(it); break; }
      }
      if (_mgr) _mgr->unlock();
    }

    void bindManager(ConfigManager* mgr) { _mgr = mgr; }

    bool resetToDefaults(ConfigManager& mgr) override {
      if (!_d.service) return false;

      mgr.lock();

      // Member defaults first: updaters treat absent keys as "unchanged"
      // (partial UI saves), so an empty-doc pass alone would reset nothing.
      // The empty pass then still runs for updaters that seed derived
      // defaults on an empty payload (e.g. security's factory users).
      _d.service->updateWithoutPropagation([](T& s) {
        s = T{};
        return StateUpdateResult::CHANGED;
      });
      DynamicJsonDocument emptyDoc(_d.bufferSize);
      JsonObject empty = emptyDoc.to<JsonObject>();
      _d.service->updateWithoutPropagation(empty, _d.updater);

      _hasLastSavedCrc = false;

      mgr.unlock();

      _stats.resets++;
      return saveIfChanged(mgr, "reset");
    }

    bool restoreFromBackup(ConfigManager& mgr) override {
      ensurePaths(mgr);

      mgr.lock();

      bool ok = mgr.restoreBackupToPrimary(_primary.c_str(), _backup.c_str(), _tmp.c_str());
      if (!ok) {
        mgr.unlock();
        return false;
      }

      mgr.unlock();

      _loaded = false;  // force reload apply
      _hasLastSavedCrc = false;

      _stats.restores++;
      return ensureLoaded(mgr);
    }

   private:
    ConfigDescriptor<T> _d;
    String _id;
    String _primary;

    String _backup;
    String _tmp;

    bool _loaded{false};

    bool _hasLastSavedCrc{false};
    uint32_t _lastSavedCrc{0};

    EntryStats _stats;

    ConfigManager* _mgr{nullptr};
    std::vector<std::pair<ListenerId, GenericListener>> _observers;
    ListenerId _nextObserverId{0};

    void ensurePaths(ConfigManager& mgr) {
      if (_backup.length() == 0) {
        if (_d.backup) _backup = String(_d.backup);
        else _backup = mgr.makeBackupPathFor(_primary.c_str());
      }
      if (_tmp.length() == 0) {
        if (_d.tmp) _tmp = String(_d.tmp);
        else _tmp = mgr.makeTmpPathFor(_primary.c_str());
      }
    }

    // autoSave removed — no automatic handler registration

    void cacheCrcFromReader(ConfigManager& mgr) {
      DynamicJsonDocument doc(_d.bufferSize);
      JsonObject data = doc.to<JsonObject>();
      if (!_d.service->read(data, _d.reader)) return;  // lock timed out: no CRC, the next save writes

      _lastSavedCrc = ConfigManager::crc32OfJson(data);
      _hasLastSavedCrc = true;

      _stats.lastCrc = _lastSavedCrc;
      _stats.lastTs = mgr.nowTs();
    }
  };

  IConfigEntry* findEntry(const char* id);

  fs::FS* _fs{nullptr};

#if CFGMGR_USE_FREERTOS
  SemaphoreHandle_t _mx{nullptr};
#endif

  // Робочі директорії. Без dot-prefix: LittleFS непередбачувано поводиться з
  // такими іменами (mkdir повертає true, але rename у "/config/.bak/..." згодом
  // провалюється). Тому завжди "/config/_tmp" і "/config/_bak".
  String _bakRoot{"/config/_bak"};
  String _tmpRoot{"/config/_tmp"};

  std::vector<std::unique_ptr<IConfigEntry>> _entries;
  std::atomic<bool> _anyDeferred{false};  // marked from any task
  std::atomic<bool> _busyRetry{false};    // noteBusy(), load/store only
  int8_t _writerJob = -1;                 // FlashWriter: writeDeferred()
  uint32_t writeDeferred();

  // Save Backup (requestSnapshot), under a spinlock in ConfigManager.cpp.
  uint8_t _snapState = 0;      // SnapState
  uint32_t _snapTs = 0;
  uint32_t _snapGen = 0;       // one per request: a run that sees a newer one starts over
  size_t _snapNext = 0;        // the queued run's next entry
  bool _snapPending = false;
  bool _snapFailed = false;    // a copy of the queued run failed
  int8_t snapshotNow();        // 1 done, 0 failed, -1 not run (lock or gate)
  bool snapshotLocked();
  bool snapshotSteps(uint32_t since);  // false: PowerGuard paused writes (still queued)
  bool snapPending() const;
  void snapFinish(uint32_t gen, bool ok);

  // Probe-mode setter — called from discoverSecretKeysFromForm to flip
  // the thread_local g_probing flag while the service's buildForm runs.
  // Implementation in ConfigManager.cpp owns the storage.
  static void setProbing(bool v);

  // Type-erased walk — collects keys whose sibling "o" string starts
  // with "secret;" into out. Used by both discoverSecretKeysFromForm
  // (template) and any future callers that already hold a JsonObject.
  static void walkAndCollectSecrets(JsonObjectConst root, std::vector<String>& out);
};

template <typename T>
inline void ConfigManager::discoverSecretKeysFromForm(ConfigDescriptor<T>& desc) {
  if (!desc.formReader || !desc.service) return;
  desc.secretKeys.clear();

  // The probe renders the WHOLE form (every tab), usually far larger than
  // the config file `bufferSize` was sized for. A truncated probe would
  // silently miss the secret keys past the overflow point and they would
  // be written to flash in plaintext (LIB-FIX-004c) — so grow until the
  // form fits. Boot-time only, heap is fresh, the doc is freed per pass.
  size_t cap = desc.bufferSize < 2048 ? 2048 : desc.bufferSize;
  bool complete = false;
  for (; cap <= 32768; cap *= 2) {
    T scratch{};
    DynamicJsonDocument doc(cap);
    if (doc.capacity() == 0) break;  // no heap — fail safe below
    JsonObject root = doc.to<JsonObject>();
    setProbing(true);
    desc.formReader(scratch, root);
    setProbing(false);
    if (doc.overflowed()) continue;
    // ArduinoJson 6.21 quirk: JsonObject::as<JsonObjectConst>() fails to
    // compile on the RISC-V toolchain — use implicit conversion via a
    // local variable instead (same trick already used by other call sites).
    JsonObjectConst rootC = root;
    walkAndCollectSecrets(rootC, desc.secretKeys);
    complete = true;
    break;
  }

  if (!complete) {
    // FAIL-SAFE: which keys are secret can't be proven, so every top-level
    // string the config reader writes is treated as one. Settings still
    // persist and load (decrypt passes plaintext through); they're simply
    // all encrypted on flash. Never a silent plaintext secret.
    log_e("[cfg] %s: secret-key probe INCOMPLETE (form > %u B or heap exhausted) — "
          "encrypting ALL string keys of this config",
          desc.id, (unsigned)cap);
    T scratch{};
    DynamicJsonDocument doc(desc.bufferSize);
    JsonObject root = doc.to<JsonObject>();
    if (desc.reader) desc.reader(scratch, root);
    for (JsonPair kv : root) {
      if (kv.value().is<const char*>()) desc.secretKeys.emplace_back(kv.key().c_str());
    }
    // Every string now travels as "ENC:" + IV + blocks in hex (~2x + 68
    // chars): give the save/load documents room for that, or saves would
    // abort (see encryptSecretsInPlace). Best effort, capped.
    const size_t grown = desc.bufferSize * 2 + 128 * desc.secretKeys.size();
    desc.bufferSize = grown > 16384 ? 16384 : grown;
  }

  if (!desc.secretKeys.empty()) {
    String joined;
    for (const String& k : desc.secretKeys) { joined += ' '; joined += k; }
    log_i("[cfg] discovered %u secret keys for %s:%s",
          (unsigned)desc.secretKeys.size(), desc.id, joined.c_str());
  }
}

#endif  // CONFIG_MANAGER_H
