#include "ConfigManager.h"

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
}  // namespace

ConfigManager::ConfigManager(fs::FS* fs) : _fs(fs) {
#if CFGMGR_USE_FREERTOS
  _mx = xSemaphoreCreateMutex();
#endif
}

void ConfigManager::begin() {
  if (!_fs) return;

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
  // so flipping the roots here is the entire change.
  _tmpRoot = "/config";
  _bakRoot = "/config";

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
    File dir = _fs->open("/config");
    if (dir && dir.isDirectory()) {
      File entry = dir.openNextFile();
      while (entry) {
        String name = entry.name();
        bool isFile = !entry.isDirectory();
        entry.close();
        if (isFile && name.endsWith(".bak")) {
          // dir.openNextFile may return either bare names or full paths
          // depending on littlefs version; normalise to absolute.
          String full = name.startsWith("/") ? name : (String("/config/") + name);
          String snap = full.substring(0, full.length() - 4) + ".snapshot";
          if (!_fs->exists(snap)) {
            if (_fs->rename(full.c_str(), snap.c_str())) {
              Serial.printf("[ConfigManager] migrated %s -> %s\n",
                            full.c_str(), snap.c_str());
            } else {
              Serial.printf("[ConfigManager] migrate %s failed\n", full.c_str());
            }
          } else {
            (void)removeIfExists(full.c_str());
          }
        }
        entry = dir.openNextFile();
      }
    }
    if (dir) dir.close();
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

  String snap = String(primaryPath) + ".snapshot";
  (void)removeIfExists(snap.c_str());
  bool ok = copyFile(primaryPath, snap.c_str());
  if (!ok) {
    Serial.printf("[CFG] snapshotEntry %s FAILED\r\n", primaryPath);
  }
  return ok;
}

bool ConfigManager::restoreEntryFromSnapshot(const char* primaryPath) {
  if (!_fs || !primaryPath || !*primaryPath) return false;

  String snap = String(primaryPath) + ".snapshot";
  if (!fileExists(snap.c_str())) return false;

  (void)removeIfExists(primaryPath);
  bool ok = copyFile(snap.c_str(), primaryPath);
  if (!ok) {
    Serial.printf("[CFG] restoreEntryFromSnapshot %s FAILED\r\n", primaryPath);
  }
  return ok;
}

bool ConfigManager::hasSnapshot(const char* primaryPath) const {
  if (!_fs || !primaryPath || !*primaryPath) return false;
  String snap = String(primaryPath) + ".snapshot";
  return _fs->exists(snap);
}

bool ConfigManager::snapshotAll() {
  // Walk every registered entry and force-copy primary → primary.snapshot.
  // Returns true only if EVERY entry succeeded (or was a no-op because the
  // primary doesn't exist yet — first-flash devices). One bad copy fails
  // the whole batch so the operator gets a single error indication.
  // Each successful copy bumps the per-entry snaps + lastSnapshotTs so
  // the Config Manager tab's Age column reflects "how fresh is THIS
  // entry's snapshot" rather than "how recently was THIS entry saved".
  uint32_t now = nowTs();
  bool allOk = true;
  forEachEntry([&](IConfigEntry& e) {
    const char* p = e.primary().c_str();
    if (!p || !*p) return;
    if (!fileExists(p)) return;            // nothing to snapshot
    if (snapshotEntry(p)) {
      e.stats().snaps++;
      e.stats().lastSnapshotTs = now;
    } else {
      allOk = false;
    }
  });
  return allOk;
}

bool ConfigManager::restoreAllFromSnapshot() {
  // Returns true only if at least one entry HAD a snapshot AND every
  // restore that ran succeeded. If no snapshots exist anywhere, this
  // is a no-op error (false) so the action's snackbar tells the
  // operator "nothing to restore" instead of silently rebooting.
  bool anySnapshot = false;
  bool allOk = true;
  forEachEntry([&](IConfigEntry& e) {
    const char* p = e.primary().c_str();
    if (!p || !*p) return;
    if (!hasSnapshot(p)) return;
    anySnapshot = true;
    if (!restoreEntryFromSnapshot(p)) allOk = false;
  });
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

bool ConfigManager::removeIfExists(const char* path) {
  if (!fileExists(path)) return true;
  return _fs->remove(path);
}

bool ConfigManager::mkdirsForFile(const char* filePath) {
  if (!_fs || !filePath) return false;
  String path(filePath);
  int idx = 0;
  while ((idx = path.indexOf('/', idx + 1)) != -1) {
    String seg = path.substring(0, idx);
    if (seg.length() && !_fs->exists(seg)) {
      (void)_fs->mkdir(seg);
    }
  }
  return true;
}

bool ConfigManager::copyFile(const char* src, const char* dst) {
  if (!_fs || !src || !dst) return false;

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

bool ConfigManager::readJsonFile(const char* path, DynamicJsonDocument& doc) {
  if (!_fs || !path) return false;
  File f = _fs->open(path, "r");
  if (!f) {
    Serial.printf("[CFG] readJsonFile: open(%s) FAILED\r\n", path);
    return false;
  }

  // No head-dump on success — leaks ENC: ciphertext, JWT secret hash,
  // and other sensitive bytes into the serial console where they
  // survive in scrollback. Only failures stay verbose.
  DeserializationError err = deserializeJson(doc, f);
  if (err) {
    Serial.printf("[CFG] readJsonFile %s deserialize err=%s\r\n", path, err.c_str());
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

void ConfigManager::encryptSecretsInPlace(JsonObject root, const std::vector<String>& keys) {
#if FT_ENABLED(FT_SECRETS_VAULT)
  for (const String& k : keys) {
    if (k.isEmpty() || !root.containsKey(k)) continue;
    String v = root[k].as<String>();
    if (v.isEmpty()) continue;
    if (SecretsVault::isEncrypted(v)) continue;  // already ENC: — idempotent
    String enc = SecretsVault::encrypt(v);
    root[k] = enc;
  }
#else
  // Encryption compiled out — settings classes still declare secretKeys
  // (auto-discovered via the form probe) but the walk is a no-op.
  // Passwords land on disk as plaintext.
  (void)root; (void)keys;
#endif
}

bool ConfigManager::decryptSecretsInPlace(JsonObject root, const std::vector<String>& keys) {
#if FT_ENABLED(FT_SECRETS_VAULT)
  bool foundPlaintext = false;
  for (const String& k : keys) {
    if (k.isEmpty() || !root.containsKey(k)) continue;
    String v = root[k].as<String>();
    if (v.isEmpty()) continue;
    if (!SecretsVault::isEncrypted(v)) {
      // Legacy plaintext on disk — leave the value as-is so the
      // service still gets its password, but flag the migration so
      // the caller forces a normalize-save (which will run encryptInPlace
      // and rewrite the file in ENC: form on the next tick).
      foundPlaintext = true;
      continue;
    }
    String dec = SecretsVault::decrypt(v);
    // decrypt() returns "" on hard failure (corrupted ciphertext / key
    // mismatch from device swap). Surface that as an empty value to
    // the service: WiFi will fail to connect, MQTT will refuse auth,
    // operator notices and retypes the credential.
    root[k] = dec;
  }
  return foundPlaintext;
#else
  // Encryption compiled out — every value loaded from disk is treated
  // as plaintext. ENC:<hex> records left over from a previous encrypted
  // build will pass through unchanged to the service (which will see
  // the literal ciphertext as the password and fail). Operator must
  // re-enter credentials after toggling the feature off.
  (void)root; (void)keys;
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
  // Is this object a SECRET-typed field? Field's `o` starts with
  // "secret;" (FieldType::SECRET → toString "secret"). When yes, the
  // sibling key in this same object that isn't "o" is the JSON key
  // that ConfigManager must encrypt on save / decrypt on load.
  const char* o = root["o"] | (const char*)nullptr;
  if (o && strncmp(o, "secret;", 7) == 0) {
    for (JsonPairConst inner : root) {
      if (strcmp(inner.key().c_str(), "o") == 0) continue;
      out.emplace_back(inner.key().c_str());
      break;  // FormBuilder writes exactly one value key per field
    }
    return;  // leaf — don't recurse further
  }
  // Not a secret leaf — recurse into nested objects and array
  // elements. Tab arrays (status/settings/scan) come through here.
  for (JsonPairConst kv : root) {
    JsonVariantConst v = kv.value();
    if (v.is<JsonObjectConst>()) {
      walkAndCollectSecrets(v.as<JsonObjectConst>(), out);
    } else if (v.is<JsonArrayConst>()) {
      for (JsonVariantConst el : v.as<JsonArrayConst>()) {
        if (el.is<JsonObjectConst>()) {
          walkAndCollectSecrets(el.as<JsonObjectConst>(), out);
        }
      }
    }
  }
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
    Serial.println(F("[CFG] verifyDoc: root is null"));
    return false;
  }
  JsonObjectConst data;
  bool legacy = false;

  if (!extractData(root, data, legacy)) {
    Serial.println(F("[CFG] verifyDoc: extractData failed"));
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
    Serial.printf("[CFG] verifyDoc: unknown algo '%s'\r\n", algo);
    return false;
  }

  uint32_t expected = 0;
  if (!hexToCrc32(hash, expected)) {
    Serial.printf("[CFG] verifyDoc: bad hash '%s'\r\n", hash);
    return false;
  }

  uint32_t got = crc32OfJson(data);

  info.crcChecked = true;
  info.ok = (got == expected);
  if (!info.ok) {
    String dataStr;
    serializeJson(data, dataStr);
    Serial.printf("[CFG] verifyDoc CRC mismatch: expected %08x got %08x len=%u\r\n",
                  (unsigned)expected, (unsigned)got, (unsigned)dataStr.length());
    Serial.printf("[CFG]   data: %s\r\n", dataStr.c_str());
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

  Serial.printf("[CFG] writeRootWithMeta wrote %u bytes (data=%u dataMemUsage=%u)\r\n",
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

  (void)removeIfExists(primary);

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

      if (!restoreBackupToPrimary(primary, backup, tmp)) {
        return LoadResult::Fail;
      }
      return LoadResult::OkRestoredFromBackup;
    }
    if (outBackup) *outBackup = vi;
  }

  outDoc.clear();
  return LoadResult::NeedDefaults;
}

bool ConfigManager::atomicWrite(const char* primary,
                                const char* backup,
                                const char* /*tmp*/,
                                JsonObjectConst data,
                                size_t bufferSize,
                                uint32_t* outCrc) {
  if (!_fs || !primary) {
    Serial.println(F("[CFG] write: null args"));
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
  // every LittleFS we target.

  mkdirsForFile(primary);

  // Best-effort backup of the previous primary BEFORE overwriting it.
  if (backup && fileExists(primary)) {
    mkdirsForFile(backup);
    (void)removeIfExists(backup);
    if (!copyFile(primary, backup)) {
      Serial.printf("[CFG] backup copy %s -> %s failed (continuing)\r\n", primary, backup);
    }
  }

  uint32_t crc = crc32OfJson(data);
  uint32_t ts  = nowTs();

  File f = _fs->open(primary, "w");
  if (!f) {
    Serial.printf("[CFG] write open %s FAILED\r\n", primary);
    return false;
  }

  bool ok = writeRootWithMeta(f, data, crc, ts);
  f.close();

  if (!ok) {
    Serial.printf("[CFG] write %s FAILED\r\n", primary);
    return false;
  }

  // Read-after-write verification — non-blocking; failure here logs but
  // doesn't roll back (the previous primary is already in `backup`,
  // operators can restore from there if the new save is corrupt).
  {
    DynamicJsonDocument vdoc(bufferSize);
    VerifyInfo vi;
    if (!readAndVerify(primary, vdoc, vi) || !vi.ok) {
      Serial.printf("[CFG] post-write verify FAILED for %s\r\n", primary);
    }
  }

  Serial.printf("[CFG] write OK %s crc=%08x\r\n", primary, (unsigned)crc);
  if (outCrc) *outCrc = crc;
  return true;
}
