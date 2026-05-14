#pragma once
#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <FS.h>
#include <ArduinoJson.h>

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
    NeedDefaults,
    Fail
  };

  // (на майбутнє) — тут можна буде накопичувати статистику
  struct EntryStats {
    uint32_t loads{0};
    uint32_t saves{0};
    uint32_t skipped{0};
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
  // restored primary cleanly.
  bool snapshotEntry(const char* primaryPath);
  bool restoreEntryFromSnapshot(const char* primaryPath);
  bool hasSnapshot(const char* primaryPath) const;
  bool snapshotAll();
  bool restoreAllFromSnapshot();

  // ---- internal helpers used by entries/templates ----
  void lock();
  void unlock();

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
  static void encryptSecretsInPlace(JsonObject root, const std::vector<String>& keys);
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
        // defaults via updater(empty)
        DynamicJsonDocument emptyDoc(_d.bufferSize);
        JsonObject empty = emptyDoc.to<JsonObject>();
        _d.service->updateWithoutPropagation(empty, _d.updater);

        _hasLastSavedCrc = false;  // force first save
        _loaded = true;


        mgr.unlock();

        _stats.loads++;
        (void)saveIfChanged(mgr, "defaults");
        return true;
      }

      if (res == LoadResult::OkPrimary || res == LoadResult::OkRestoredFromBackup) {
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

        // Decrypt every declared secretKey IN PLACE before the updater
        // sees the JSON. Returns true if at least one secretKey was
        // plaintext on disk — that's the legacy / first-boot upgrade
        // path that needs a normalize-save to encrypt the file.
        bool foundPlaintext = ConfigManager::decryptSecretsInPlace(in, _d.secretKeys);

        _d.service->updateWithoutPropagation(in, _d.updater);

        cacheCrcFromReader(mgr);

        bool needNormalize = false;
        if (legacy) needNormalize = true;
        if (!legacy && !vP.hadMeta) needNormalize = true;
        if (res == LoadResult::OkRestoredFromBackup) needNormalize = true;
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
      if (!_d.service) return false;

      ensurePaths(mgr);

      mgr.lock();

      DynamicJsonDocument dataDoc(_d.bufferSize);
      JsonObject data = dataDoc.to<JsonObject>();
      _d.service->read(data, _d.reader);

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
        mgr.unlock();
        return true;  // skip flash write
      }

      // Encrypt secret fields IN PLACE just before atomicWrite — the
      // file on disk gets the ENC:<hex> form, but the in-memory state
      // (and any subsequent reader() call) stays plaintext.
      ConfigManager::encryptSecretsInPlace(data, _d.secretKeys);

      // Was a primary file already on disk before this save? If so,
      // atomicWrite() rotates it into <primary>.snapshot, which counts
      // as a snapshot write — surface that on the Config Manager tab
      // by bumping snaps + lastSnapshotTs. First-save-ever (no primary
      // yet) skips the snapshot bookkeeping because no file rotated.
      bool primaryExisted = mgr.fileExists(_primary.c_str());

      uint32_t written = 0;
      bool ok = mgr.atomicWrite(_primary.c_str(), _backup.c_str(), _tmp.c_str(), data, _d.bufferSize, &written);

      std::vector<std::pair<ListenerId, GenericListener>> snap;
      if (ok) {
        // Track plaintext CRC for skip detection. The on-disk CRC
        // (`written`) goes into stats.lastCrc for diagnostic display.
        _lastSavedCrc = crcPlain;
        _hasLastSavedCrc = true;

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
      _d.service->read(data, _d.reader);

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
  T scratch{};
  DynamicJsonDocument doc(desc.bufferSize);
  JsonObject root = doc.to<JsonObject>();
  setProbing(true);
  desc.formReader(scratch, root);
  setProbing(false);

  desc.secretKeys.clear();
  // ArduinoJson 6.21 quirk: JsonObject::as<JsonObjectConst>() fails to
  // compile on the RISC-V toolchain — use implicit conversion via a
  // local variable instead (same trick already used by other call sites).
  JsonObjectConst rootC = root;
  walkAndCollectSecrets(rootC, desc.secretKeys);

  if (!desc.secretKeys.empty()) {
    Serial.print(F("[cfg] discovered "));
    Serial.print(desc.secretKeys.size());
    Serial.print(F(" secret keys for "));
    Serial.print(desc.id);
    Serial.print(F(":"));
    for (const String& k : desc.secretKeys) { Serial.print(' '); Serial.print(k); }
    Serial.println();
  }
}

#endif  // CONFIG_MANAGER_H
