#include "ConfigManager.h"
#include "SecretFields.h"

#include <strings.h>  // strcasecmp

// ---------- small crc sink ----------
namespace {
class Crc32Sink : public Print {
 public:
  uint32_t crc = 0xFFFFFFFFUL;

  size_t write(uint8_t b) override {
    crc = update(crc, b);
    return 1;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    for (size_t i = 0; i < size; i++) crc = update(crc, buffer[i]);
    return size;
  }

  uint32_t final() const { return crc ^ 0xFFFFFFFFUL; }

 private:
  static uint32_t update(uint32_t c, uint8_t b) {
    c ^= b;
    for (int i = 0; i < 8; i++) {
      if (c & 1)
        c = (c >> 1) ^ 0xEDB88320UL;
      else
        c >>= 1;
    }
    return c;
  }
};

// GateHeld: the tasks that hold a write gate slot or pass for their config
// work (static: DRAM, a spinlock, no atomics in heap objects).
#if CFGMGR_USE_FREERTOS
constexpr uint8_t kHeldTasks = 4;
struct HeldTask {
  TaskHandle_t task;
  uint8_t depth;
};
HeldTask s_held[kHeldTasks] = {};
portMUX_TYPE s_heldMux = portMUX_INITIALIZER_UNLOCKED;
#else
uint8_t s_heldDepth = 0;
#endif

// Save Backup state (ConfigManager::_snap*): any task.
#if CFGMGR_USE_FREERTOS
portMUX_TYPE s_snapMux = portMUX_INITIALIZER_UNLOCKED;
#define CFG_SNAP_LOCK() portENTER_CRITICAL(&s_snapMux)
#define CFG_SNAP_UNLOCK() portEXIT_CRITICAL(&s_snapMux)
#else
#define CFG_SNAP_LOCK()
#define CFG_SNAP_UNLOCK()
#endif

// The restart closed the config writes to every task but its own
// (ConfigManager::closeForRestart); under s_snapMux.
#if CFGMGR_USE_FREERTOS
void* s_closedFor = nullptr;
#endif

// Marks a Save Backup run under way: still there at boot, the run was cut
// (a power cut, a crash) and the snapshots are of two moments.
constexpr const char* kSnapRunMarker = "/config/snapshot.partial";

// A failed deferred save is tried again after these (the last one repeats).
constexpr uint32_t kFailRetryMs[] = {1000, 2000, 5000, 10000, 30000, 60000};
constexpr uint8_t kFailRetrySteps = sizeof(kFailRetryMs) / sizeof(kFailRetryMs[0]);

// One helper step (or a short run of them) through the write gate: nothing
// to take when the task holds the gate already (GateHeld); otherwise a pass
// of its own, never waiting, with the task marked for the nested helpers.
// onlyFree: several flash operations, only while the gate is free.
// PowerGuard decides either way: under GateHeld it is asked per step (writes
// may have been paused since the caller got its slot).
class GateStep {
 public:
  explicit GateStep(bool onlyFree)
      : _own(!ConfigManager::GateHeld::here()),
        _pass(_own && ESPRack::WriteGate::tryEnter(ESPRack::WriteGate::now(), onlyFree, &_at)),
        _mark(_pass) {}
  ~GateStep() {
    if (_pass) ESPRack::WriteGate::leave(_at);
  }
  bool ok() const { return _own ? _pass : ESPRack::PowerGuard::writesAllowed(); }
  GateStep(const GateStep&) = delete;
  GateStep& operator=(const GateStep&) = delete;

 private:
  uint32_t _at = 0;  // before _pass: set by tryEnter in _pass's initialiser
  bool _own;
  bool _pass;
  ConfigManager::GateHeld _mark;
};
}  // namespace

ConfigManager::GateHeld::GateHeld(bool on) {
  if (!on) return;
#if CFGMGR_USE_FREERTOS
  TaskHandle_t t = xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&s_heldMux);
  HeldTask* spare = nullptr;
  for (HeldTask& h : s_held) {
    if (h.task == t) {
      ++h.depth;
      _on = true;
      break;
    }
    if (!h.task && !spare) spare = &h;
  }
  if (!_on && spare) {
    spare->task = t;
    spare->depth = 1;
    _on = true;
  }
  portEXIT_CRITICAL(&s_heldMux);
#else
  ++s_heldDepth;
  _on = true;
#endif
}

ConfigManager::GateHeld::~GateHeld() {
  if (!_on) return;
#if CFGMGR_USE_FREERTOS
  TaskHandle_t t = xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&s_heldMux);
  for (HeldTask& h : s_held) {
    if (h.task != t) continue;
    if (--h.depth == 0) h.task = nullptr;
    break;
  }
  portEXIT_CRITICAL(&s_heldMux);
#else
  --s_heldDepth;
#endif
}

bool ConfigManager::GateHeld::here() {
#if CFGMGR_USE_FREERTOS
  TaskHandle_t t = xTaskGetCurrentTaskHandle();
  bool held = false;
  portENTER_CRITICAL(&s_heldMux);
  for (const HeldTask& h : s_held) {
    if (h.task == t) {
      held = true;
      break;
    }
  }
  portEXIT_CRITICAL(&s_heldMux);
  return held;
#else
  return s_heldDepth > 0;
#endif
}

ConfigManager::ConfigManager(fs::FS* fs) : _fs(fs) {
#if CFGMGR_USE_FREERTOS
  _mx = xSemaphoreCreateMutex();
#endif
}

void ConfigManager::begin() {
  if (!_fs) return;
  if (_writerJob < 0) {
    _writerJob = ESPRack::FlashWriter::add("config", [this] { return writeDeferred(); });
    // Saves then go past a busy gate (saveStep); a queued Save Backup fails.
    if (_writerJob < 0) log_e("[ConfigManager] no FlashWriter job left: saves go past the write gate");
  }

  // Same-directory staging, whatever the housekeeping below manages.
  _tmpRoot = "/config";
  _bakRoot = "/config";

  // A Save Backup run cut short by the last reset (a read: before the gate).
  if (_fs->exists(kSnapRunMarker)) {
    CFG_SNAP_LOCK();
    _snapState = static_cast<uint8_t>(SnapState::Interrupted);
    CFG_SNAP_UNLOCK();
    log_w("[ConfigManager] the last Save Backup was cut short: the backup files are of two moments");
  }

  // The housekeeping below writes (mkdir, rmdir, renames): one write gate
  // pass for it, while the gate is free (at boot, before any module holds
  // windows). Not let in: skipped this boot, nothing is lost (the first
  // save creates /config; the migration runs on a later boot).
  GateStep gate(true);
  if (!gate.ok()) {
    log_w("[ConfigManager] begin: flash busy (write gate) — housekeeping skipped");
    return;
  }

  if (!_fs->exists("/config")) {
    (void)_fs->mkdir("/config");
  }

  // Pre-Arduino-3.x ESP32 used /config/_tmp + /config/_bak subdirectories
  // for atomic-write staging; rename(tmp → primary) and rename(primary →
  // backup) crossed those directory boundaries. The IDF 5 LittleFS
  // implementation (Arduino-3.x / pioarduino on S3 / C3 / C6) refuses
  // cross-directory rename — fs->rename() returns false. Result on the
  // wire: atomicWrite step 4 fails, save is rolled back, snackbar still
  // says "Update successful" because frontend only knows the POST
  // returned 200, but on next reboot the device starts with default
  // (empty) credentials. Reproduced on C6: [wifi] saveIfChanged(http) -> 0.
  //
  // Fix: keep tmp + backup files in the SAME /config directory using
  // suffix paths (`foo.json.tmp`, `foo.json.bak`). Within-directory
  // rename is guaranteed atomic on every LittleFS version we target.
  // makeTmpPathFor() / makeBackupPathFor() already special-case
  // `_tmpRoot == "/config"` and emit `<primary>.tmp` / `<primary>.bak`,
  // so flipping the roots (above) is the entire change.

  // Best-effort cleanup of orphaned subdirs from older firmwares so
  // they don't waste flash blocks. Failures here are non-fatal.
  if (_fs->exists("/config/_tmp")) {
    (void)_fs->rmdir("/config/_tmp");  // littlefs rmdir only works when empty;
  }                                    // any leftover *.tmp files are reused/overwritten anyway
  if (_fs->exists("/config/_bak")) {
    (void)_fs->rmdir("/config/_bak");
  }

  // Migrate previous-firmware `.bak` files to the unified `.snapshot`
  // suffix. Operators upgrading don't lose their last auto-saved
  // recovery point — and once the rename runs once, every reboot finds
  // no `.bak` files and the loop is a no-op. Best-effort throughout —
  // an error here just means one entry boots without a snapshot, which
  // the next save will repopulate via atomicWrite's auto-rotate.
  {
    // Listed by name (no File per entry: each open File costs a 4 KB
    // buffer in internal RAM), renamed after the listing is closed.
    // Also the .tmp copies a power cut left (copyReplace, a restore):
    // nothing writes them before begin().
    std::vector<String> bak;
    std::vector<String> tmp;
    File dir = _fs->open("/config");
    if (dir && dir.isDirectory()) {
      bool isDir = false;
      for (String name = dir.getNextFileName(&isDir); name.length(); name = dir.getNextFileName(&isDir)) {
        if (!isDir && name.endsWith(".bak")) bak.push_back(name);
        if (!isDir && name.endsWith(".tmp")) tmp.push_back(name);
      }
    }
    if (dir) dir.close();
    for (const String& name : tmp) {
      (void)removeIfExists((name.startsWith("/") ? name : (String("/config/") + name)).c_str());
    }
    for (const String& name : bak) {
      // The listing may give bare names or full paths depending on the
      // littlefs version; normalise to absolute.
      String full = name.startsWith("/") ? name : (String("/config/") + name);
      String snap = full.substring(0, full.length() - 4) + ".snapshot";
      if (!_fs->exists(snap)) {
        if (_fs->rename(full.c_str(), snap.c_str())) {
          log_d("[ConfigManager] migrated %s -> %s",
                        full.c_str(), snap.c_str());
        } else {
          log_e("[ConfigManager] migrate %s failed", full.c_str());
        }
      } else {
        (void)removeIfExists(full.c_str());
      }
    }
  }

  // tmpRoot/bakRoot diagnostic was useful while the snapshot mode
  // was new; it's stable now and just adds noise on every boot.
}

void ConfigManager::lock() {
#if CFGMGR_USE_FREERTOS
  if (_mx) xSemaphoreTake(_mx, portMAX_DELAY);
#endif
}
void ConfigManager::unlock() {
#if CFGMGR_USE_FREERTOS
  if (_mx) xSemaphoreGive(_mx);
#endif
}
bool ConfigManager::tryLock(uint32_t ms) {
#if CFGMGR_USE_FREERTOS
  if (_mx) return xSemaphoreTake(_mx, pdMS_TO_TICKS(ms)) == pdTRUE;
#endif
  (void)ms;
  return true;
}

void ConfigManager::closeForRestart() {
#if CFGMGR_USE_FREERTOS
  CFG_SNAP_LOCK();
  s_closedFor = xTaskGetCurrentTaskHandle();
  CFG_SNAP_UNLOCK();
#endif
  // A step another task has under way ends first: each looks under the lock.
  if (tryLock(CFGMGR_LOCK_WAIT_MS)) {
    unlock();
  } else {
    log_w("[CFG] restart: a config step still holds the lock");
  }
}

bool ConfigManager::restartClosed() const {
#if CFGMGR_USE_FREERTOS
  CFG_SNAP_LOCK();
  void* t = s_closedFor;
  CFG_SNAP_UNLOCK();
  return t && t != static_cast<void*>(xTaskGetCurrentTaskHandle());
#else
  return false;  // one task: the restart's own
#endif
}

uint32_t ConfigManager::restartFlushMs(uint32_t boundMs) const {
  // Per save: the slot, the lock, the two files. A queued Save Backup: a
  // slot and a flash step each for the copy and the rename per file and
  // for the run's marker (in and out), and one wait for the lock.
  constexpr uint32_t kFileMs = 500;
  uint32_t ms = (uint32_t)deferredCount() * (boundMs + CFGMGR_LOCK_WAIT_MS + 2 * kFileMs);
  if (snapPending()) ms += ((uint32_t)_entries.size() * 2 + 2) * (boundMs + kFileMs) + CFGMGR_LOCK_WAIT_MS;
  return ms;
}

uint32_t ConfigManager::nowTs() const {
  return (uint32_t)(millis() / 1000UL);
}

ConfigManager::IConfigEntry* ConfigManager::findEntry(const char* id) {
  if (!id || !*id) return nullptr;
  for (auto& e : _entries) {
    if (e && e->id().equals(id)) return e.get();
  }
  return nullptr;
}

ConfigManager::EntryStats* ConfigManager::statsOf(const char* id) {
  auto* e = findEntry(id);
  if (!e) return nullptr;
  return &e->stats();
}

bool ConfigManager::ensureLoaded(const char* id) {
  auto* e = findEntry(id);
  if (!e) return false;
  return e->ensureLoaded(*this);
}

bool ConfigManager::saveIfChanged(const char* id, const String& origin) {
  auto* e = findEntry(id);
  if (!e) return false;
  return e->saveIfChanged(*this, origin);
}

size_t ConfigManager::deferredCount() const {
  size_t n = 0;
  for (const auto& e : _entries) {
    if (e && e->deferred()) ++n;
  }
  return n;
}

void ConfigManager::flushDeferred() {
  if (_anyDeferred) ESPRack::FlashWriter::kick(_writerJob);
}

// The FlashWriter task: each deferred save in two steps, a gate slot each
// (the backup copy, then the file), then a queued Save Backup. The config
// lock is never held while waiting for a slot: a save from async_tcp
// meanwhile isn't kept waiting. Each step waits on a clock of its own: only
// a step that itself found no pause for maxWaitMs + maxWindowMs goes
// forced, not every step after the first 12 s of a long run.
uint32_t ConfigManager::writeDeferred() {
  bool power = false;
  bool failed = false;
  _busyRetry.store(false);
  for (auto& e : _entries) {
    if (!e || !e->deferred()) continue;
    if (restartClosed()) {  // the restart writes them (flushDeferredNow)
      power = true;
      break;
    }
    // The backup copy, unless there is none to make (made already for this
    // save, no file yet, or a stale primary): then no slot for it.
    IConfigEntry::Backup b = IConfigEntry::Backup::Made;
    if (e->backupDue(*this)) {
      ESPRack::WriteSlot slot;
      if (!slot.ok()) {
        power = true;
        break;
      }
      GateHeld mark;  // the slot covers the backup copy's helpers
      b = e->backupForSave(*this);
    }
    if (b == IConfigEntry::Backup::Paused) {
      power = true;
      break;
    }
    if (b == IConfigEntry::Backup::Failed) {  // no copy: the file is not rewritten in place
      failed = true;
      continue;
    }
    if (b != IConfigEntry::Backup::Made) continue;  // Unneeded, or Busy (noteBusy)
    ESPRack::WriteSlot slot;
    if (!slot.ok()) {
      power = true;
      break;
    }
    if (!e->saveStep(*this, "deferred", true, true) && e->deferred()) {
      if (!ESPRack::PowerGuard::writesAllowed()) {
        power = true;
        break;
      }
      failed = true;
    }
  }
  _anyDeferred = deferredCount() > 0;
  // After the saves, so the snapshot freezes what they wrote (a save that
  // failed doesn't hold it up: it freezes what is on flash).
  // Stopped, not for the lock (noteBusy): PowerGuard or the restart.
  if (!power && snapPending() && !snapshotSteps()) power = !_busyRetry.load();
  if (!_anyDeferred && !snapPending()) {
    _failStreak.store(0);
    return ESPRack::FlashWriter::kDone;
  }
  // PowerGuard paused writes (before or during a step), or the restart
  // took over: run again when PowerGuard allows writes (or on the next kick).
  if (power) return ESPRack::FlashWriter::kOnPower;
  // A service's state lock or the config lock timed out (noteBusy): again
  // in CFGMGR_BUSY_RETRY_MS.
  if (!failed && _busyRetry.load()) return CFGMGR_BUSY_RETRY_MS;
  // A save that failed (a flash error, no internal RAM for a file buffer, a
  // full file system) or left over: again later, each time a little later.
  // No PowerGuard event would come for it.
  const uint8_t n = _failStreak.load();
  const uint32_t ms = kFailRetryMs[n < kFailRetrySteps ? n : kFailRetrySteps - 1];
  if (n < 255) _failStreak.store(static_cast<uint8_t>(n + 1));
  log_w("[CFG] %u deferred save(s) not written: try #%u in %u ms", (unsigned)deferredCount(), (unsigned)(n + 1),
        (unsigned)ms);
  return ms;
}

void ConfigManager::flushDeferredNow(uint32_t boundMs) {
  // From here on only this task writes the config: the FlashWriter's run
  // and a late save on another task stop (restartClosed()).
  closeForRestart();
  for (auto& e : _entries) {
    if (!e || !e->deferred()) continue;
    ESPRack::WriteSlot slot(boundMs);
    if (!slot.ok()) {  // PowerGuard: kept until it allows writes, which the restart won't wait for
      log_w("[CFG] %s: deferred save not written before the restart (writes paused)", e->id().c_str());
      continue;
    }
    if (!e->saveStep(*this, "deferred", true, false) && e->deferred()) {  // makes its own backup copy
      log_w("[CFG] %s: deferred save not written before the restart", e->id().c_str());
    }
  }
  _anyDeferred = deferredCount() > 0;
  // A queued Save Backup: its remaining steps, a bounded wait each, or it
  // would go with the restart.
  if (snapPending() && !snapshotSteps(boundMs)) {
    log_w("[CFG] queued snapshot not written before the restart (writes paused or config busy)");
  }
}

bool ConfigManager::restoreFromBackup(const char* id) {
  auto* e = findEntry(id);
  if (!e) return false;
  return e->restoreFromBackup(*this);
}

bool ConfigManager::resetToDefaults(const char* id) {
  auto* e = findEntry(id);
  if (!e) return false;
  return e->resetToDefaults(*this);
}

// ---- snapshot operations ----------------------------------------------------
// `<primary>.snapshot` plays both the auto-rotate recovery role (atomicWrite
// makes the copy before opening primary for write) and the operator-visible
// "freeze current state" role triggered from the Config Manager tab.

bool ConfigManager::snapshotEntry(const char* primaryPath) {
  if (!_fs || !primaryPath || !*primaryPath) return false;
  if (!fileExists(primaryPath)) return false;

  // Through <snapshot>.tmp: a cut leaves the old snapshot or the new one.
  String snap = String(primaryPath) + ".snapshot";
  bool ok = copyReplace(primaryPath, snap.c_str());
  if (!ok) {
    log_e("[CFG] snapshotEntry %s FAILED", primaryPath);
  }
  return ok;
}

// Only from a snapshot that verifies (a torn or empty one is skipped), and
// through <primary>.tmp, verified again, then renamed over the primary: the
// primary is never removed before a verified copy is there.
bool ConfigManager::restoreEntryFromSnapshot(const char* primaryPath, size_t bufferSize) {
  if (!_fs || !primaryPath || !*primaryPath) return false;

  String snap = String(primaryPath) + ".snapshot";
  if (!fileExists(snap.c_str())) return false;
  if (!verifyFile(snap.c_str(), bufferSize)) {
    log_e("[CFG] restore %s: the snapshot fails to verify — primary kept", primaryPath);
    return false;
  }
  const String tmp = makeTmpPathFor(primaryPath);
  bool ok = copyFile(snap.c_str(), tmp.c_str()) && verifyFile(tmp.c_str(), bufferSize) &&
            replaceWith(tmp.c_str(), primaryPath);
  if (!ok) {
    (void)removeIfExists(tmp.c_str());
    log_e("[CFG] restoreEntryFromSnapshot %s FAILED — primary kept", primaryPath);
  }
  return ok;
}

ConfigManager::SnapCheck ConfigManager::checkSnapshots() {
  if (!tryLock(CFGMGR_LOCK_WAIT_MS)) return SnapCheck::Busy;
  bool any = false;
  bool bad = false;
  forEachEntry([&](IConfigEntry& e) {
    const char* p = e.primary().c_str();
    if (!p || !*p || !hasSnapshot(p)) return;
    any = true;
    const String snap = e.primary() + ".snapshot";
    if (!verifyFile(snap.c_str(), e.bufferSize())) {
      log_w("[CFG] %s fails to verify", snap.c_str());
      bad = true;
    }
  });
  unlock();
  return !any ? SnapCheck::None : bad ? SnapCheck::Bad : SnapCheck::Ok;
}

bool ConfigManager::hasSnapshot(const char* primaryPath) const {
  if (!_fs || !primaryPath || !*primaryPath) return false;
  String snap = String(primaryPath) + ".snapshot";
  return _fs->exists(snap);
}

bool ConfigManager::dumpAll(JsonObject out) {
  // Read each registered entry's primary file verbatim (encrypted
  // secrets stay encrypted on the wire) and stash under the entry's
  // id key. A read miss on any entry fails the whole dump — caller
  // gets a single error instead of a half-populated payload.
  if (!_fs) return false;
  bool ok = true;
  forEachEntry([&](IConfigEntry& e) {
    if (!ok) return;
    const char* p = e.primary().c_str();
    if (!p || !*p) return;
    if (!fileExists(p)) {
      // Entry registered but never saved — emit an empty object
      // rather than skipping, so restoreAll can still trigger a
      // "wipe back to defaults" by passing an empty sub-object.
      out.createNestedObject(e.id());
      return;
    }
    File f = _fs->open(p, "r");
    if (!f) { ok = false; return; }
    DynamicJsonDocument doc(8192);
    DeserializationError jerr = deserializeJson(doc, f);
    f.close();
    if (jerr) { ok = false; return; }
    JsonObject slot = out.createNestedObject(e.id());
    slot.set(doc.as<JsonVariantConst>());
  });
  return ok;
}

size_t ConfigManager::restoreAll(JsonObjectConst in) {
  // Write each entry's primary file from the matching sub-object in
  // `in`. Atomic per-entry (atomicWrite via the existing snapshot/
  // tmp/primary rotation). Returns count of entries actually written
  // — caller usually reboots after a non-zero count so each service's
  // begin() picks up the new files cleanly.
  if (!_fs) return 0;
  size_t written = 0;
  forEachEntry([&](IConfigEntry& e) {
    const char* p = e.primary().c_str();
    if (!p || !*p) return;
    JsonVariantConst slot = in[e.id().c_str()];
    if (slot.isNull() || !slot.is<JsonObjectConst>()) return;
    JsonObjectConst envelope = slot.as<JsonObjectConst>();
    // The dumped payload is {"data": {...}, "meta": {...}}; restore
    // only the "data" field through atomicWrite so meta (crc + ts)
    // gets re-computed for the destination filesystem rather than
    // restored verbatim from the source device.
    JsonObjectConst dataConst;
    bool legacy = false;
    if (!extractData(envelope, dataConst, legacy)) return;
    String backupPath = makeBackupPathFor(p);
    String tmpPath    = makeTmpPathFor(p);
    uint32_t outCrc = 0;
    bool ok = atomicWrite(p,
                          e.primaryStale() ? nullptr : backupPath.c_str(),
                          tmpPath.c_str(),
                          dataConst,
                          8192,
                          &outCrc);
    if (ok) {
      written++;
      e.stats().restores++;
    }
  });
  return written;
}

bool ConfigManager::snapshotLocked() {
  // Walk every registered entry and force-copy primary → primary.snapshot.
  // Returns true only if EVERY entry succeeded (or was a no-op because the
  // primary doesn't exist yet — first-flash devices). One bad copy fails
  // the whole batch so the operator gets a single error indication.
  // Each successful copy bumps the per-entry snaps + lastSnapshotTs so
  // the Config Manager tab's Age column reflects "how fresh is THIS
  // entry's snapshot" rather than "how recently was THIS entry saved".
  // The caller holds the lock and the write gate (GateHeld). The run's
  // marker is there while it goes (a cut: the page says so after the boot).
  uint32_t now = nowTs();
  bool allOk = true;
  (void)markSnapshotRun(true);
  forEachEntry([&](IConfigEntry& e) {
    const char* p = e.primary().c_str();
    if (!p || !*p) return;
    if (!fileExists(p)) return;            // nothing to snapshot
    if (e.primaryStale()) {                // failed to verify: the good backup stays
      log_w("[CFG] snapshot %s skipped: primary not repaired yet", p);
      allOk = false;
      return;
    }
    if (snapshotEntry(p)) {
      e.stats().snaps++;
      e.stats().lastSnapshotTs = now;
    } else {
      allOk = false;
    }
  });
  (void)markSnapshotRun(false);
  return allOk;
}

int8_t ConfigManager::snapshotNow() {
  if (!tryLock(CFGMGR_LOCK_WAIT_MS)) return -1;
  int8_t r = -1;
  {
    GateStep gate(true);  // ~2 flash steps per entry: only while the gate is free
    if (gate.ok()) r = snapshotLocked() ? 1 : 0;
  }
  unlock();
  return r;
}

bool ConfigManager::snapshotAll() {
  return snapshotNow() > 0;
}

bool ConfigManager::snapPending() const {
  CFG_SNAP_LOCK();
  const bool p = _snapPending;
  CFG_SNAP_UNLOCK();
  return p;
}

ConfigManager::SnapState ConfigManager::snapshotState() const {
  CFG_SNAP_LOCK();
  const uint8_t s = _snapState;
  CFG_SNAP_UNLOCK();
  return static_cast<SnapState>(s);
}

uint32_t ConfigManager::snapshotStateTs() const {
  CFG_SNAP_LOCK();
  const uint32_t ts = _snapTs;
  CFG_SNAP_UNLOCK();
  return ts;
}

// A run for request `gen` ended: the outcome, unless a newer request came
// meanwhile (that one stays queued and the job starts it over).
void ConfigManager::snapFinish(uint32_t gen, bool ok) {
  const uint32_t ts = nowTs();
  CFG_SNAP_LOCK();
  if (gen == _snapGen) {
    _snapPending = false;
    _snapState = static_cast<uint8_t>(ok ? SnapState::Done : SnapState::Failed);
    _snapTs = ts;
  }
  CFG_SNAP_UNLOCK();
}

ConfigManager::SnapState ConfigManager::requestSnapshot() {
  // Saves still waiting for the flash: the snapshot goes after them.
  if (!hasDeferred() && !snapPending()) {
    const int8_t r = snapshotNow();
    if (r >= 0 || _writerJob < 0) {
      const SnapState s = r > 0 ? SnapState::Done : SnapState::Failed;
      const uint32_t ts = nowTs();
      CFG_SNAP_LOCK();
      ++_snapGen;
      _snapState = static_cast<uint8_t>(s);
      _snapTs = ts;
      CFG_SNAP_UNLOCK();
      return s;
    }
  }
  const uint32_t ts = nowTs();
  CFG_SNAP_LOCK();
  ++_snapGen;  // (again) from the first entry: "now" is this request
  _snapNext = 0;
  _snapFailed = false;
  _snapPending = true;
  _snapState = static_cast<uint8_t>(SnapState::Queued);
  _snapTs = ts;
  CFG_SNAP_UNLOCK();
  ESPRack::WriteGate::noteDeferred();
  ESPRack::FlashWriter::kick(_writerJob);
  return SnapState::Queued;
}

// The queued Save Backup, on the FlashWriter task (or the restart's flush:
// boundMs). Per entry two flash steps, a gate slot each: the primary copied
// to <snapshot>.tmp, then renamed over the snapshot (a cut leaves the old
// snapshot or the new one). One step more each for the run's marker, at the
// start and the end. Each step under the lock, which is never held while
// waiting for a slot. false: it stays queued from the entry it stopped at
// (PowerGuard paused writes, the restart took over, or the lock was busy:
// noteBusy).
bool ConfigManager::snapshotSteps(uint32_t boundMs) {
  // One step: 1 done, 0 failed, -1 not run (no slot, the restart, busy).
  auto step = [&](auto fn) -> int8_t {
    ESPRack::WriteSlot slot(boundMs);
    if (!slot.ok()) return -1;
    if (!tryLock(CFGMGR_LOCK_WAIT_MS)) {
      noteBusy();
      return -1;
    }
    if (restartClosed()) {
      unlock();
      return -1;
    }
    bool ok;
    {
      GateHeld mark;
      ok = fn();
    }
    unlock();
    return ok ? 1 : 0;
  };
  for (;;) {
    CFG_SNAP_LOCK();
    const bool pending = _snapPending;
    const uint32_t gen = _snapGen;
    size_t i = _snapNext;
    CFG_SNAP_UNLOCK();
    if (!pending) return true;  // finished meanwhile (the restart's flush)
    if (i == 0 && !fileExists(kSnapRunMarker) && step([&] { return markSnapshotRun(true); }) < 0) return false;
    bool newer = false;
    for (; i < _entries.size() && !newer; ++i) {
      IConfigEntry* e = _entries[i].get();
      if (!e) continue;
      const String p = e->primary();
      if (!p.length() || !fileExists(p.c_str())) continue;  // nothing to snapshot
      if (e->primaryStale()) {  // failed to verify: the good backup stays
        log_w("[CFG] snapshot %s skipped: primary not repaired yet", p.c_str());
        CFG_SNAP_LOCK();
        if (gen == _snapGen) {
          _snapNext = i + 1;
          _snapFailed = true;
        } else {
          newer = true;
        }
        CFG_SNAP_UNLOCK();
        continue;
      }
      const String snap = p + ".snapshot";
      const String tmp = snap + ".tmp";
      int8_t r = step([&] { return fileExists(p.c_str()) && copyFile(p.c_str(), tmp.c_str()); });
      if (r > 0) r = step([&] { return replaceWith(tmp.c_str(), snap.c_str()); });
      if (r < 0) {  // resume at this entry
        CFG_SNAP_LOCK();
        if (gen == _snapGen) _snapNext = i;
        CFG_SNAP_UNLOCK();
        return false;
      }
      const bool copied = r > 0;
      if (copied) {
        e->stats().snaps++;
        e->stats().lastSnapshotTs = nowTs();
      } else {
        if (fileExists(tmp.c_str())) (void)step([&] { return removeIfExists(tmp.c_str()); });
        log_e("[CFG] snapshotEntry %s FAILED", p.c_str());
      }
      CFG_SNAP_LOCK();
      if (gen == _snapGen) {
        _snapNext = i + 1;
        if (!copied) _snapFailed = true;
      } else {
        newer = true;  // a new request: start over
      }
      CFG_SNAP_UNLOCK();
    }
    if (newer) continue;
    if (fileExists(kSnapRunMarker) && step([&] { return markSnapshotRun(false); }) < 0) return false;
    CFG_SNAP_LOCK();
    const bool failed = _snapFailed;
    CFG_SNAP_UNLOCK();
    snapFinish(gen, !failed);
    if (!snapPending()) return true;  // else a request came after the last step
  }
}

bool ConfigManager::restoreAllFromSnapshot() {
  // Returns true only if at least one entry HAD a snapshot AND every
  // restore that ran succeeded. If no snapshots exist anywhere, this
  // is a no-op error (false) so the action's snackbar tells the
  // operator "nothing to restore" instead of silently rebooting.
  // Under the lock and the write gate (the restart's after-shutdown step
  // holds a slot: then no pass of its own).
  if (!tryLock(CFGMGR_LOCK_WAIT_MS)) {
    log_e("[CFG] restore from snapshot: config busy — not restored");
    return false;
  }
  bool anySnapshot = false;
  bool allOk = true;
  {
    GateStep gate(true);
    if (!gate.ok()) {
      unlock();
      log_e("[CFG] restore from snapshot: flash busy or writes paused — not restored");
      return false;
    }
    forEachEntry([&](IConfigEntry& e) {
      const char* p = e.primary().c_str();
      if (!p || !*p) return;
      if (!hasSnapshot(p)) return;
      anySnapshot = true;
      if (!restoreEntryFromSnapshot(p, e.bufferSize())) allOk = false;  // skipped: its primary stays
    });
  }
  unlock();
  return anySnapshot && allOk;
}

String ConfigManager::makeBackupPathFor(const char* primary) const {
  // The backup file is now `<primary>.snapshot` — same auto-rotated
  // recovery semantics that `.bak` used to have, plus the operator
  // can override the contents via the Config Manager tab. Single
  // suffix, two writers (auto on save, manual via snapshotAll).
  String p(primary ? primary : "");
  return p + ".snapshot";
}

String ConfigManager::makeTmpPathFor(const char* primary) const {
  String p(primary ? primary : "");
  if (p.startsWith("/config/")) {
    if (_tmpRoot == "/config") {
      return p + ".tmp";
    }
    String rel = p.substring(strlen("/config/"));
    return _tmpRoot + "/" + rel + ".tmp";
  }
  return String(primary) + ".tmp";
}

bool ConfigManager::fileExists(const char* path) const {
  return _fs && path && _fs->exists(path);
}

// The file helpers: one write gate step each, unless the task holds the
// gate already (GateHeld). Turned away: false, like a flash error.
bool ConfigManager::removeIfExists(const char* path) {
  if (!fileExists(path)) return true;
  GateStep gate(false);
  if (!gate.ok()) {
    log_w("[CFG] remove %s: flash busy (write gate)", path);
    return false;
  }
  return _fs->remove(path);
}

bool ConfigManager::mkdirsForFile(const char* filePath) {
  if (!_fs || !filePath) return false;
  String path(filePath);
  int idx = 0;
  while ((idx = path.indexOf('/', idx + 1)) != -1) {
    String seg = path.substring(0, idx);
    if (seg.length() && !_fs->exists(seg)) {
      GateStep gate(false);
      if (gate.ok()) (void)_fs->mkdir(seg);  // else: the open that needs it fails
    }
  }
  return true;
}

bool ConfigManager::copyFile(const char* src, const char* dst) {
  if (!_fs || !src || !dst) return false;
  GateStep gate(false);  // one file of a few KB
  if (!gate.ok()) {
    log_w("[CFG] copy %s: flash busy (write gate)", dst);
    return false;
  }

  File in = _fs->open(src, "r");
  if (!in) return false;

  mkdirsForFile(dst);
  File out = _fs->open(dst, "w");
  if (!out) {
    in.close();
    return false;
  }

  uint8_t buf[1024];
  while (in.available()) {
    size_t n = in.read(buf, sizeof(buf));
    if (!n) break;
    if (out.write(buf, n) != n) {
      in.close();
      out.close();
      return false;
    }
  }

  in.close();
  out.close();
  return true;
}

// LittleFS renames over an existing file atomically (same directory): the
// old dst or the new one is there after a cut, never neither.
bool ConfigManager::replaceWith(const char* tmp, const char* dst) {
  if (!_fs || !tmp || !dst) return false;
  GateStep gate(false);  // a directory update
  if (!gate.ok()) {
    log_w("[CFG] rename to %s: flash busy (write gate)", dst);
    return false;
  }
  if (_fs->rename(tmp, dst)) return true;
  log_e("[CFG] rename %s -> %s failed", tmp, dst);
  return false;
}

bool ConfigManager::copyReplace(const char* src, const char* dst) {
  if (!src || !dst) return false;
  const String tmp = String(dst) + ".tmp";
  if (copyFile(src, tmp.c_str()) && replaceWith(tmp.c_str(), dst)) return true;
  (void)removeIfExists(tmp.c_str());
  return false;
}

bool ConfigManager::verifyFile(const char* path, size_t bufferSize) {
  if (!fileExists(path)) return false;
  DynamicJsonDocument doc(bufferSize);
  if (doc.capacity() == 0) return false;  // no heap: not known to be good
  VerifyInfo vi;
  return readAndVerify(path, doc, vi) && vi.ok;
}

bool ConfigManager::markSnapshotRun(bool on) {
  if (!on) return removeIfExists(kSnapRunMarker);
  if (fileExists(kSnapRunMarker)) return true;
  GateStep gate(false);  // an empty file
  if (!gate.ok()) return false;
  File f = _fs->open(kSnapRunMarker, "w");
  if (!f) return false;
  f.close();
  return true;
}

bool ConfigManager::readJsonFile(const char* path, DynamicJsonDocument& doc) {
  if (!_fs || !path) return false;
  File f = _fs->open(path, "r");
  if (!f) {
    log_e("[CFG] readJsonFile: open(%s) FAILED", path);
    return false;
  }

  // No head-dump on success — leaks ENC: ciphertext, JWT secret hash,
  // and other sensitive bytes into the serial console where they
  // survive in scrollback. Only failures stay verbose.
  DeserializationError err = deserializeJson(doc, f);
  if (err) {
    log_d("[CFG] readJsonFile %s deserialize err=%s", path, err.c_str());
  }
  f.close();

  if (err != DeserializationError::Ok) return false;
  if (!doc.is<JsonObject>()) return false;
  return true;
}

uint32_t ConfigManager::crc32OfJson(JsonObjectConst data) {
  Crc32Sink sink;
  serializeJson(data, sink);
  return sink.final();
}

String ConfigManager::crc32ToHex(uint32_t crc) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%08lX", (unsigned long)crc);
  return String(buf);
}

bool ConfigManager::hexToCrc32(const char* hex, uint32_t& out) {
  if (!hex || !*hex) return false;
  char* end = nullptr;
  unsigned long v = strtoul(hex, &end, 16);
  if (end == hex) return false;
  out = (uint32_t)v;
  return true;
}

// ---- Field-level encryption walk (Part D) -----------------------------
//
// Both helpers operate on the JsonObject `root` after extractData has
// returned the inner data object — so dot-paths aren't needed; the
// secretKeys list contains plain top-level keys for now. (If a future
// service needs nested-field encryption, extend the walk to split on
// '.' and traverse into nested objects.)
//
// Empty values pass through unchanged in both directions: an empty
// password should stay empty rather than become "ENC:<some bytes>",
// because UI logic distinguishes "no password set" from "password
// configured" via emptiness checks.

// false: a ciphertext didn't fit the document (the value is then null) —
// the caller must not write this document to flash.
bool ConfigManager::encryptSecretsInPlace(JsonObject root, const std::vector<String>& keys) {
#if FT_ENABLED(FT_SECRETS_VAULT)
  bool ok = true;
  for (const String& k : keys) {
    if (k.isEmpty() || !root.containsKey(k)) continue;
    String v = root[k].as<String>();
    if (v.isEmpty()) continue;
    if (SecretsVault::isEncrypted(v)) continue;  // already ENC: — idempotent
    String enc = SecretsVault::encrypt(v);
    if (!root[k].set(enc)) {
      log_e("[CFG] secret '%s' doesn't fit the save buffer once encrypted — save aborted", k.c_str());
      ok = false;
    }
  }
  return ok;
#else
  // Encryption compiled out — settings classes still declare secretKeys
  // (auto-discovered via the form probe) but the walk is a no-op.
  // Passwords land on disk as plaintext.
  (void)root; (void)keys;
  return true;
#endif
}

namespace {
bool listedKey(const std::vector<String>& keys, const char* k) {
  for (const String& x : keys) {
    if (x == k) return true;
  }
  return false;
}
}  // namespace

// Returns true when the file must be rewritten (normalize-save):
//  * a secret key was plaintext on disk (legacy / first boot) -> encrypt it;
//  * a NON-secret key was in ENC: form. The probe's fail-safe encrypts every
//    top-level string when it can't prove which keys are secret; a later boot
//    whose probe succeeds knows only the real secrets, so those values must
//    still be decrypted here (by shape, not by the key list) or the service
//    would get the literal "ENC:..." text (e.g. as its SSID). The rewrite
//    stores them plain again.
bool ConfigManager::decryptSecretsInPlace(JsonObject root, const std::vector<String>& keys) {
#if FT_ENABLED(FT_SECRETS_VAULT)
  bool needRewrite = false;
  for (JsonPair kv : root) {
    if (!kv.value().is<const char*>()) continue;
    const String v = kv.value().as<String>();
    if (v.isEmpty()) continue;
    const bool secret = listedKey(keys, kv.key().c_str());
    if (secret ? !SecretsVault::isEncrypted(v) : !SecretsVault::looksEncrypted(v)) {
      // Legacy plaintext secret on disk: leave it so the service still gets
      // its password, and have the caller rewrite the file encrypted.
      if (secret) needRewrite = true;
      continue;
    }
    String dec = SecretsVault::decrypt(v);
    // decrypt() returns "" on hard failure (corrupted ciphertext / key
    // mismatch from device swap). Surface that as an empty value to
    // the service: WiFi will fail to connect, MQTT will refuse auth,
    // operator notices and retypes the credential. The ENC: record on
    // disk stays untouched (cleared in RAM only, no rewrite for this key)
    // so flashing the firmware that owns the key back onto this flash
    // still recovers the original value.
    if (dec.isEmpty()) {
      log_w("[CFG] '%s' failed to decrypt (foreign device key or corrupt record) — cleared; re-enter via UI",
            kv.key().c_str());
      kv.value().set("");  // a literal: stored by pointer, always fits
      continue;
    }
    if (!kv.value().set(dec)) {  // the load document is full
      log_e("[CFG] '%s' doesn't fit the load buffer once decrypted — cleared", kv.key().c_str());
      kv.value().set("");
      continue;
    }
    if (!secret) needRewrite = true;  // fail-safe residue: store it plain again
  }
  return needRewrite;
#else
  // Encryption compiled out — values load as plaintext, with ONE
  // exception: ENC:<hex> residue written by a vault-enabled build (any
  // top-level string, see above). The literal blob must never reach a
  // service as a live credential — the WiFi driver rejects >64-char
  // passphrases in an endless connect loop and softAP silently truncates to
  // a password nobody knows, either of which strands the device. Clear it in
  // RAM only: the record stays on disk so a vault-enabled firmware flashed
  // back later still recovers the value. Operator re-enters it to use this
  // build.
  (void)keys;
  for (JsonPair kv : root) {
    if (!kv.value().is<const char*>()) continue;
    if (SecretsVault::looksEncrypted(kv.value().as<String>())) {
      log_w("[CFG] '%s' is vault-encrypted but FT_SECRETS_VAULT=0 — cleared; re-enter via UI", kv.key().c_str());
      kv.value().set("");
    }
  }
  return false;
#endif
}

// ---- Probe-mode signal ----------------------------------------------
// Set true while ConfigManager is running a service's buildForm to
// auto-discover SECRET fields. Plain `static` rather than thread_local
// because thread_local on RISC-V toolchain crashes (Store access fault
// to 0x0) when accessed during global static init — the FreeRTOS task
// TLS slot hasn't been bootstrapped yet at that moment, but our setter
// runs from registerConfig which is invoked from ConfigDelegate
// attach() inside service ctors that are themselves member-initialised
// inside the global ESPReact ctor.
//
// Plain static is safe here because the probe is single-threaded by
// construction: registerConfig runs only on the main task during early
// init, never concurrently from another task. If a future caller wants
// to probe from a different task, that's the place to add a mutex (or
// per-call parameter) — not undo this fix.
namespace { static bool g_probing = false; }
bool ConfigManager::isProbing() { return g_probing; }
void ConfigManager::setProbing(bool v) { g_probing = v; }

// ---- Type-erased walk ---------------------------------------------
// FormBuilder field shape: each field is a JsonObject sitting directly
// inside a tab's JsonArray, e.g.
//   { "settings": [ { "pwd": "...", "o": "secret;rw;..." }, ... ] }
// → so SECRET detection runs against THIS object first (not "look at
// nested objects"), and recursion happens through arrays + nested
// objects only when the current object isn't itself a secret leaf.
void ConfigManager::walkAndCollectSecrets(JsonObjectConst root, std::vector<String>& out) {
  // Single definition of "secret field" shared with the web layer's
  // masking (SecretFields.h): a field whose `o` starts with "secret;".
  SecretFields::collectFromForm(root, out);
}

// root either:
// 1) {"_meta":{...}, "data":{...}}  (new)
// 2) {...}                          (legacy)
bool ConfigManager::extractData(JsonVariantConst root, JsonObjectConst& dataOut, bool& legacyOut) {
  legacyOut = false;

  // ArduinoJson 6.21 quirk on RISC-V (C3 / C6): JsonVariantConst::is<JsonObject>()
  // can return false even for a valid object value. as<JsonObjectConst>().isNull()
  // is the reliable shape check across every architecture we target.
  JsonObjectConst obj = root.as<JsonObjectConst>();
  if (obj.isNull()) return false;

  JsonObjectConst vData = obj["data"].as<JsonObjectConst>();
  JsonObjectConst vMeta = obj["_meta"].as<JsonObjectConst>();

  if (!vData.isNull()) {
    dataOut = vData;
    legacyOut = false;
    return true;
  }

  if (!vMeta.isNull()) {
    return false;  // new format but missing data => broken
  }

  dataOut = obj;
  legacyOut = true;
  return true;
}

bool ConfigManager::verifyDoc(const DynamicJsonDocument& doc, VerifyInfo& info) {
  info = VerifyInfo{};
  // ArduinoJson 6.21 + RISC-V (ESP32-C3 / C6) has a quirk where
  // `doc.is<JsonObject>()` returns false even for a valid root object;
  // `as<JsonObjectConst>().isNull()` is the reliable check on every
  // architecture/core combination. Symptom we hit: a 175 B file with a
  // perfectly valid {"_meta":{…},"data":{…}} round-tripped through
  // open/deserialize without errors, yet `is<JsonObject>()` returned
  // false → every save was reported as "verify FAILED".
  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.isNull()) {
    log_d("[CFG] verifyDoc: root is null");
    return false;
  }
  JsonObjectConst data;
  bool legacy = false;

  if (!extractData(root, data, legacy)) {
    log_e("[CFG] verifyDoc: extractData failed");
    return false;
  }
  info.legacy = legacy;

  if (legacy) {
    info.ok = true;
    info.hadMeta = false;
    info.crcChecked = false;
    return true;
  }

  // Same RISC-V is<JsonObject>() quirk applies here — use isNull check.
  JsonObjectConst meta = root["_meta"].as<JsonObjectConst>();
  if (meta.isNull()) {
    // new-ish but no meta => allow, normalize later
    info.ok = true;
    info.hadMeta = false;
    info.crcChecked = false;
    return true;
  }

  info.hadMeta = true;
  const char* algo = meta["algo"] | "";
  const char* hash = meta["hash"] | "";

  if (strcasecmp(algo, "crc32") != 0) {
    log_d("[CFG] verifyDoc: unknown algo '%s'", algo);
    return false;
  }

  uint32_t expected = 0;
  if (!hexToCrc32(hash, expected)) {
    log_d("[CFG] verifyDoc: bad hash '%s'", hash);
    return false;
  }

  uint32_t got = crc32OfJson(data);

  info.crcChecked = true;
  info.ok = (got == expected);
  if (!info.ok) {
    String dataStr;
    serializeJson(data, dataStr);
    log_w("[CFG] verifyDoc CRC mismatch: expected %08x got %08x len=%u",
                  (unsigned)expected, (unsigned)got, (unsigned)dataStr.length());
    log_d("[CFG]   data: %s", dataStr.c_str());
  }
  return info.ok;
}

bool ConfigManager::readAndVerify(const char* path, DynamicJsonDocument& doc, VerifyInfo& info) {
  if (!readJsonFile(path, doc)) return false;
  return verifyDoc(doc, info);
}

bool ConfigManager::writeRootWithMeta(File& f, JsonObjectConst data, uint32_t crc, uint32_t ts) {
  if (!f) return false;

  String hashStr = crc32ToHex(crc);

  size_t total = 0;
  size_t n = 0;

  n = f.print("{\"_meta\":{\"v\":1,\"algo\":\"crc32\",\"hash\":\""); total += n;
  n = f.print(hashStr);                                            total += n;
  n = f.print("\",\"ts\":");                                       total += n;
  n = f.print(ts);                                                 total += n;
  n = f.print("},\"data\":");                                      total += n;
  n = serializeJson(data, f);                                      total += n;
  n = f.print("}");                                                total += n;

  log_d("[CFG] writeRootWithMeta wrote %u bytes (data=%u dataMemUsage=%u)",
                (unsigned)total, (unsigned)n,
                (unsigned)data.memoryUsage());
  return total > 0;
}

static String fallbackTmpSameDir(const char* primary) {
  return String(primary ? primary : "") + ".tmp";
}

bool ConfigManager::restoreBackupToPrimary(const char* primary, const char* backup, const char* tmp) {
  if (!_fs || !primary || !backup || !tmp) return false;
  if (!fileExists(backup)) return false;
  // Several flash steps (a test file, copies, a rename): only while the
  // write gate is free, unless the caller holds it.
  GateStep gate(true);
  if (!gate.ok()) {
    log_w("[CFG] restore %s from backup: flash busy (write gate)", primary);
    return false;
  }

  // tmp може бути в директорії, яку неможливо створити (як у твоєму логу)
  String tmpPath(tmp);
  mkdirsForFile(tmpPath.c_str());

  // якщо все одно не дає — fallback у той же каталог
  File test = _fs->open(tmpPath.c_str(), "w");
  if (!test) {
    tmpPath = fallbackTmpSameDir(primary);
    mkdirsForFile(tmpPath.c_str());
  } else {
    test.close();
    (void)removeIfExists(tmpPath.c_str());
  }

  if (!copyFile(backup, tmpPath.c_str())) return false;

  // Renamed over the primary (no remove first: a cut leaves one of them).
  bool ok = _fs->rename(tmpPath.c_str(), primary);
  if (!ok) {
    ok = copyFile(tmpPath.c_str(), primary);
    (void)removeIfExists(tmpPath.c_str());
  }
  return ok;
}

ConfigManager::LoadResult ConfigManager::loadOrRecover(const char* primary,
                                                       const char* backup,
                                                       const char* tmp,
                                                       size_t bufferSize,
                                                       DynamicJsonDocument& outDoc,
                                                       VerifyInfo* outPrimary,
                                                       VerifyInfo* outBackup) {
  if (!_fs || !primary || !backup || !tmp) return LoadResult::Fail;

  // очистити tmp (якщо існує)
  (void)removeIfExists(tmp);

  // primary
  {
    VerifyInfo vi;
    outDoc.clear();

    if (fileExists(primary) && readAndVerify(primary, outDoc, vi) && vi.ok) {
      if (outPrimary) *outPrimary = vi;
      return LoadResult::OkPrimary;
    }
    if (outPrimary) *outPrimary = vi;
  }

  // backup
  {
    VerifyInfo vi;
    outDoc.clear();

    if (fileExists(backup) && readAndVerify(backup, outDoc, vi) && vi.ok) {
      if (outBackup) *outBackup = vi;

      // The copy turned away (write gate, PowerGuard) or failed: run on
      // the verified backup all the same; the primary is rewritten by the
      // next save, which leaves the backup as it is (OkBackupOnly).
      if (!restoreBackupToPrimary(primary, backup, tmp)) {
        log_w("[CFG] %s: running on the backup, primary not repaired yet", primary);
        return LoadResult::OkBackupOnly;
      }
      return LoadResult::OkRestoredFromBackup;
    }
    if (outBackup) *outBackup = vi;
  }

  outDoc.clear();
  return LoadResult::NeedDefaults;
}

bool ConfigManager::backupPrimary(const char* primary, const char* backup) {
  if (!backup || !primary || !fileExists(primary)) return true;  // nothing to keep
  mkdirsForFile(backup);
  // Through <backup>.tmp: a failed or cut copy never costs the backup there is.
  if (copyReplace(primary, backup)) return true;
  log_e("[CFG] backup copy %s -> %s failed: %s not rewritten", primary, backup, primary);
  return false;
}

bool ConfigManager::atomicWrite(const char* primary,
                                const char* backup,
                                const char* /*tmp*/,
                                JsonObjectConst data,
                                size_t bufferSize,
                                uint32_t* outCrc,
                                bool* outBackupHeld) {
  if (outBackupHeld) *outBackupHeld = false;
  if (!ESPRack::PowerGuard::writesAllowed()) {  // any other writer
    log_w("[CFG] write %s skipped: writes paused (PowerGuard)", primary ? primary : "?");
    return false;
  }
  if (!_fs || !primary) {
    log_d("[CFG] write: null args");
    return false;
  }
  // The backup copy, the file and its read-back: only while the write gate
  // is free, unless the caller holds it (a save: its pass or slot).
  GateStep gate(true);
  if (!gate.ok()) {
    log_w("[CFG] write %s: flash busy (write gate)", primary);
    return false;
  }

  // The previous tmp/rename atomic pipeline broke on Arduino-3.x LittleFS:
  // either the tmp file came back empty after `f.print()` or the
  // cross-directory rename returned false. Net result — every save
  // (WiFi/AP/NTP/MQTT/AutoUpdate/Telegram/LightState) silently failed
  // and only OTA survived because it still uses the legacy FSPersistence
  // pipeline (single open(W) → serializeJson → close).
  //
  // Mirror that proven path here: write straight to `primary`, with a
  // best-effort copy of the previous primary to `backup` first so
  // operators have at least one rollback file on disk. Atomicity is
  // weaker than the old design (a power loss mid-write loses the
  // current primary; the .bak is the recovery point), but it works on
  // every LittleFS we target. The backup is always there first (or there
  // was no primary): backupPrimary goes through a .tmp and a rename.

  mkdirsForFile(primary);

  // The backup of the previous primary BEFORE overwriting it (null: done
  // already, as the FlashWriter's first step). Without it the primary is
  // not rewritten in place: a cut then would leave neither.
  if (!backupPrimary(primary, backup)) return false;
  // The copy is made (or there was nothing to copy): from here on a retry
  // must not copy this primary over it again.
  if (outBackupHeld && backup) *outBackupHeld = true;
  if (!ESPRack::PowerGuard::writesAllowed()) {  // paused since: the backup copy is all
    log_w("[CFG] write %s skipped: writes paused (PowerGuard)", primary);
    return false;
  }

  uint32_t crc = crc32OfJson(data);
  uint32_t ts  = nowTs();

  File f = _fs->open(primary, "w");
  if (outBackupHeld) *outBackupHeld = true;  // the primary may be torn now
  if (!f) {
    log_e("[CFG] write open %s FAILED", primary);
    return false;
  }

  bool ok = writeRootWithMeta(f, data, crc, ts);
  f.close();

  if (!ok) {
    log_e("[CFG] write %s FAILED", primary);
    return false;
  }

  // Read-after-write verification — non-blocking; failure here logs but
  // doesn't roll back (the previous primary is already in `backup`,
  // operators can restore from there if the new save is corrupt).
  {
    DynamicJsonDocument vdoc(bufferSize);
    VerifyInfo vi;
    if (!readAndVerify(primary, vdoc, vi) || !vi.ok) {
      log_e("[CFG] post-write verify FAILED for %s", primary);
    }
  }

  log_i("[CFG] write OK %s crc=%08x", primary, (unsigned)crc);
  if (outCrc) *outCrc = crc;
  return true;
}
