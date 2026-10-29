#include <ConfigManagerService.h>

#include <AdaptiveJson.h>
#include <ESPFS.h>
#include <FormBuilder.h>
#include <WebManager.h>
#include <power/PowerGuard.h>
#include <App.h>  // ESPRack::restartDevice (LIB-FIX-006)

ConfigManagerService::ConfigManagerService(AsyncWebServer* server,
                                           SecurityManager* securityManager,
                                           ConfigManager* configManager)
    : _server(server), _sm(securityManager), _cfg(configManager) {
  // Route binding moved into registerManifest (WebPageSpec) so the
  // endpoint flows through WebManager like every other bespoke page.
  (void)_server; (void)_sm;
}

void ConfigManagerService::registerManifest(WebManager* web) {
  if (!web) return;
  using namespace std::placeholders;

  // Register the form-read endpoint via WebManager so rest.proxy can
  // reach CONFIG_MGR_FORM_PATH (/api/v1/system/configmgr/form) without
  // HTTP loopback.
  WebPageSpec page;
  page.id        = "configManagerUi";
  page.title     = "Config Manager";
  page.component = "";   // no top-level page route; contributes a tab below
  page.auth      = WebAuthLevel::Admin;
  {
    WebEndpointMeta e;
    e.method  = "GET"; e.path = CONFIG_MGR_FORM_PATH; e.role = "form";
    e.auth    = WebAuthLevel::Admin;
    e.handler = std::bind(&ConfigManagerService::serveForm, this, _1);
    e.internalHandler =
      [this](JsonVariant /*in*/, JsonVariant out, int& status) {
        JsonObject o = out.to<JsonObject>();
        buildForm(o);
        status = 200;
      };
    page.endpoints.push_back(std::move(e));
  }
  web->registerPage(std::move(page));

  // Tab attached to the compound "system" feature shell.
  WebTabSpec tab;
  tab.key      = "configmgr";
  tab.title    = "Config Manager";
  tab.restPath = CONFIG_MGR_FORM_PATH;
  tab.postable = false;
  tab.live     = false;
  tab.auth     = WebAuthLevel::Admin;
  tab.order    = 30;
  web->addTabToFeature("system", tab);

  // Action: explicit "snapshot all current primaries" — overwrites every
  // existing <primary>.snapshot with the CURRENT primary. The form's
  // refetchForm() decorator on the embedded button refreshes the tab so
  // the table's Snap column flips to ✓ without operator reload.
  // async_tcp never waits for the flash: with the write gate free it is
  // done here (200), otherwise queued for the flash writer (202) and the
  // "Last backup" row tells queued / saved / failed.
  WebActionSpec snap;
  snap.id             = "config.snapshot";
  snap.title          = "Save Backup";
  snap.icon           = "Save";
  snap.color          = "primary";
  snap.auth           = WebAuthLevel::Admin;
  snap.successMessage = "Backup saved, or queued until the flash is free (see Last backup)";
  snap.handler        = [this](AsyncWebServerRequest* r) {
    using S = ConfigManager::SnapState;
    const S s = _cfg ? _cfg->requestSnapshot() : S::Failed;
    r->send(s == S::Done ? 200 : s == S::Queued ? 202 : 500);
  };
  web->registerAction(snap);

  // Action: explicit "restore all from snapshot" — copies every
  // <primary>.snapshot over its primary, then reboots so each service's
  // begin() reloads cleanly. Returns 500 if no snapshots exist anywhere
  // (snackbar communicates "nothing to restore" without rebooting into
  // an empty-default state).
  WebActionSpec restore;
  restore.id             = "config.restore";
  restore.title          = "Restore Backup";
  restore.icon           = "SettingsBackupRestore";
  restore.color          = "warning";
  restore.auth           = WebAuthLevel::Admin;
  restore.confirm        = "Restore all settings from saved backup and reboot?";
  restore.successMessage = "Restoring snapshot, rebooting…";
  restore.handler        = [this](AsyncWebServerRequest* r) {
    // Nothing to restore -> 500 without rebooting (snackbar says so). A
    // backup file that fails to verify (torn by a power cut) -> 409, no
    // restart: the restore would skip it anyway.
    using C = ConfigManager::SnapCheck;
    const C c = _cfg ? _cfg->checkSnapshots() : C::None;
    if (c == C::None) {
      r->send(500);
      return;
    }
    if (c == C::Bad) {
      r->send(409, "application/json", "{\"ok\":false,\"error\":\"a backup file is damaged: Save Backup first\"}");
      return;
    }
    if (c == C::Busy) {
      r->send(503, "application/json", "{\"ok\":false,\"error\":\"config busy\"}");
      return;
    }
    // The restore writes the files: while PowerGuard pauses writes the
    // request is refused (503), no restart (the writes would be skipped).
    if (!ESPRack::PowerGuard::writesAllowed()) {
      r->send(503, "application/json", "{\"ok\":false,\"error\":\"flash writes paused\"}");
      return;
    }
    // The restore itself runs AFTER every module's onShutdown()
    // (LIB-FIX-006), so a module persisting state on shutdown can't
    // overwrite the restored files.
    ConfigManager* cfg = _cfg;
    r->onDisconnect([cfg]() {
      ESPRack::restartDevice(ESPRack::kRestartConfigRestore, [cfg]() { (void)cfg->restoreAllFromSnapshot(); });
    });
    r->send(200);
  };
  web->registerAction(restore);
}

void ConfigManagerService::serveForm(AsyncWebServerRequest* request) {
  sendAdaptiveJson(request, [this](JsonObject& o){ buildForm(o); }, 2560);
}

void ConfigManagerService::buildForm(JsonObject& root) {
  JsonArray cm = FormBuilder::createForm(root, "configmgr", "Config Manager");

  if (!_cfg) {
    FormBuilder::addMessageField(cm, "err",
        "Config Manager is not wired up.",
        level("error"), icon("Warning"));
    return;
  }

  // Pre-walk to compute summary stats so the header rows can render
  // colour-coded badges before the table.
  size_t totalEntries  = 0;
  size_t snapshottedEntries = 0;
  _cfg->forEachEntry([&](ConfigManager::IConfigEntry& e) {
    totalEntries++;
    const char* p = e.primary().c_str();
    if (p && *p && _cfg->hasSnapshot(p)) snapshottedEntries++;
  });

  // Summary rows
  String totalStr = String(totalEntries) + " configs registered";
  FormBuilder::addTextField(cm, "total", AF::R, totalStr.c_str(), icon("Storage"));

  String snapStr = String(snapshottedEntries) + " / " + String(totalEntries) + " snapshotted";
  // colorMap matches the literal "<N> / <N> snapshotted" strings as
  // success once snapshottedEntries == totalEntries; default → warning.
  String fullKey = String(totalEntries) + " / " + String(totalEntries) + " snapshotted";
  String mapSpec = fullKey + ":success,default:warning";
  FormBuilder::addTextField(cm, "snap_summary", AF::R, snapStr.c_str(),
                            icon("SettingsBackupRestore"),
                            colorMap(mapSpec.c_str()));

  // The last Save Backup of this boot: done at once, or queued for the
  // flash writer while the write gate is busy, then saved or failed.
  const char* lastSnap = "Not this boot";
  switch (_cfg->snapshotState()) {
    case ConfigManager::SnapState::Queued: lastSnap = "Queued (waiting for the flash)"; break;
    case ConfigManager::SnapState::Done:   lastSnap = "Saved"; break;
    case ConfigManager::SnapState::Failed: lastSnap = "Failed"; break;
    case ConfigManager::SnapState::Interrupted:
      lastSnap = "Cut short by a reset (files of two moments)";
      break;
    default: break;
  }
  FormBuilder::addTextField(cm, "snap_last", AF::R, lastSnap, icon("Save"), label("Last backup"),
                            colorMap("Saved:success,Failed:error,Not this boot:info,default:warning"));

  // Saves the write gate put off, and whether they fail (the flash writer
  // tries those again, later each time).
  String pending = "None";
  if (const size_t n = _cfg->deferredCount()) {
    const uint8_t tries = _cfg->saveRetries();
    pending = String((unsigned)n);
    if (tries) {
      pending += String(" failing, tried ") + String((unsigned)tries) + " times";
    } else {
      pending += " waiting for the flash";
    }
  }
  FormBuilder::addTextField(cm, "saves_pending", AF::R, pending.c_str(), icon("Schedule"), label("Pending saves"),
                            colorMap("None:success,default:warning"));

  // Per-entry table — populated inline with current state.
  // Two distinct counters and two file-size columns:
  //   saves     — successful saveIfChanged calls (atomicWrite OK)
  //   snaps     — successful snapshot rotations (auto on save with a
  //               pre-existing primary, plus manual Save Backup)
  //   size      — bytes of the live primary file
  //   snap_size — bytes of the .snapshot file (compare with `size` to
  //               see whether the snapshot is stale relative to live)
  //   snap_age  — seconds since the most recent snapshot write,
  //               derived from EntryStats.lastSnapshotTs (NOT lastTs,
  //               which only updates on save and skipped saves).
  JsonObject tbl = FormBuilder::addTableField(cm, "entries", AF::R,
      col("id",        "ID",        "text"),
      col("file",      "File",      "text"),
      col("size",      "Size",      "number", format("0")),
      col("saves",     "Saves",     "number", format("0")),
      col("snap",      "Snap",      "text"),
      col("snap_size", "Snap size", "number", format("0")),
      col("snaps",     "Snaps",     "number", format("0")),
      col("snap_age",  "Age (s)",   "number", format("0")),
      icon("ListAlt"));

  JsonArray rows = tbl["entries"].as<JsonArray>();
  uint32_t now = _cfg->nowTs();
  _cfg->forEachEntry([&](ConfigManager::IConfigEntry& e) {
    JsonObject row = rows.createNestedObject();
    String prim    = e.primary();
    String snapPath = prim + ".snapshot";

    row["id"]   = e.id();
    row["file"] = prim;

    // Sizes: stat each file via FS. Open(read) + size() is cheap on
    // littlefs (single inode lookup) and gives the operator immediate
    // visual feedback that snapshot reflects the same live state — if
    // size and snap_size diverge, the operator knows save-since-snapshot
    // happened.
    size_t fileSize = 0;
    if (_cfg->fileExists(prim.c_str())) {
      File f = ESPFS.open(prim.c_str(), "r");
      if (f) { fileSize = f.size(); f.close(); }
    }
    row["size"] = (uint32_t)fileSize;

    bool   hasSnap  = _cfg->fileExists(snapPath.c_str());
    size_t snapSize = 0;
    if (hasSnap) {
      File f = ESPFS.open(snapPath.c_str(), "r");
      if (f) { snapSize = f.size(); f.close(); }
    }
    row["snap"]      = hasSnap ? "yes" : "no";
    row["snap_size"] = (uint32_t)snapSize;

    row["saves"] = e.stats().saves;
    row["snaps"] = e.stats().snaps;

    // Age in seconds — from lastSnapshotTs. Reads "0" until the first
    // snapshot lands (auto-rotate on second save, or manual Save Backup,
    // whichever comes first).
    uint32_t age = 0;
    if (hasSnap && e.stats().lastSnapshotTs > 0 && now > e.stats().lastSnapshotTs) {
      age = now - e.stats().lastSnapshotTs;
    }
    row["snap_age"] = age;
  });

  // Footer actions — actionRef pulls URL/icon/color/confirm/successMessage
  // from the manifest entries registered in registerManifest().
  FormBuilder::addActionField(cm, "a_save",    "Save Backup",    AF::RW,
                              actionRef("config.snapshot"), refetchForm());
  FormBuilder::addActionField(cm, "a_restore", "Restore Backup", AF::RW,
                              actionRef("config.restore"));
}
