#include <ConfigManagerService.h>

#include <ESPFS.h>
#include <FormBuilder.h>
#include <WebManager.h>

ConfigManagerService::ConfigManagerService(AsyncWebServer* server,
                                           SecurityManager* securityManager,
                                           ConfigManager* configManager)
    : _server(server), _sm(securityManager), _cfg(configManager) {
  if (!_server || !_sm) return;
  _server->on(CONFIG_MGR_FORM_PATH,
              HTTP_GET,
              _sm->wrapRequest(std::bind(&ConfigManagerService::serveForm, this, std::placeholders::_1),
                               AuthenticationPredicates::IS_ADMIN));
}

void ConfigManagerService::registerManifest(WebManager* web) {
  if (!web) return;

  // Tab attached to the compound "system" feature shell.
  WebTabSpec tab;
  tab.key      = "configmgr";
  tab.title    = "Config Manager";
  tab.restPath = CONFIG_MGR_FORM_PATH;
  tab.postable = false;
  tab.live     = false;
  tab.auth     = WebAuthLevel::Admin;
  web->addTabToFeature("system", tab);

  // Action: explicit "snapshot all current primaries" — overwrites every
  // existing <primary>.snapshot with the CURRENT primary. The form's
  // refetchForm() decorator on the embedded button refreshes the tab so
  // the table's Snap column flips to ✓ without operator reload.
  WebActionSpec snap;
  snap.id             = "config.snapshot";
  snap.title          = "Save Backup";
  snap.icon           = "Save";
  snap.color          = "primary";
  snap.auth           = WebAuthLevel::Admin;
  snap.successMessage = "Snapshot saved — current configs frozen as backup";
  snap.handler        = [this](AsyncWebServerRequest* r) {
    bool ok = _cfg && _cfg->snapshotAll();
    r->send(ok ? 200 : 500);
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
    if (!_cfg || !_cfg->restoreAllFromSnapshot()) {
      r->send(500);
      return;
    }
    r->onDisconnect([]() {
      delay(500);
      ESP.restart();
    });
    r->send(200);
  };
  web->registerAction(restore);
}

void ConfigManagerService::serveForm(AsyncWebServerRequest* request) {
  AsyncJsonResponse* response = new AsyncJsonResponse(false, CONFIG_MGR_BUFFER_SIZE);
  JsonObject root = response->getRoot();
  buildForm(root);
  response->setLength();
  request->send(response);
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
                              actionRef("config.snapshot"));
  FormBuilder::addActionField(cm, "a_restore", "Restore Backup", AF::RW,
                              actionRef("config.restore"));
}
