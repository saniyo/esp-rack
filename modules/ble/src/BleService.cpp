#include <BleService.h>

#include <DeviceIdentity.h>
#include <WebManager.h>
#include <soc/soc_caps.h>
#include <system/BootGuard.h>
#include <system/LoopWait.h>
#include <App.h>  // ESPRack::wakeLoop

#include <new>

// The stack: NimBLE-Arduino 2.x from the consumer's lib_deps, on a chip with
// a BLE radio. Without it the module builds as a stub (see BleModule.h).
#if __has_include(<NimBLEDevice.h>) && defined(SOC_BLE_SUPPORTED) && SOC_BLE_SUPPORTED
#include <NimBLEDevice.h>
#define ESPRACK_HAVE_NIMBLE 1
#else
#define ESPRACK_HAVE_NIMBLE 0
#endif

using ESPRack::BleLinkSpec;
using ESPRack::BleLinkState;
using ESPRack::BleLinkStats;
using ESPRack::BleScanRow;

namespace {

#if ESPRACK_HAVE_NIMBLE && defined(CONFIG_BT_NIMBLE_MAX_CONNECTIONS)
constexpr uint8_t kMaxLinks = ESPRACK_BLE_MAX_LINKS < CONFIG_BT_NIMBLE_MAX_CONNECTIONS
                                  ? ESPRACK_BLE_MAX_LINKS
                                  : CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
#else
constexpr uint8_t kMaxLinks = ESPRACK_BLE_MAX_LINKS;
#endif
constexpr uint32_t kMirrorMs = 1000;
constexpr uint32_t kRssiPeriodMs = 5000;
constexpr uint8_t kForgetDiscoveredAfter = 3;  // failures before a discovered peer is re-discovered

// Static task stack: a contiguous heap block of this size would fragment the
// no-PSRAM heap (module-guide §11).
StackType_t s_stack[ESPRACK_BLE_TASK_STACK];
StaticTask_t s_tcb;

void copyStr(char* dst, const char* src, size_t n) {
  if (!n) return;
  strlcpy(dst, src ? src : "", n);
}

bool prefixMatch(const char* csv, const char* name) {
  if (!csv[0] || !name[0]) return false;
  const char* p = csv;
  while (*p) {
    const char* end = strchr(p, ',');
    const size_t len = end ? size_t(end - p) : strlen(p);
    if (len && strncmp(name, p, len) == 0) return true;
    if (!end) break;
    p = end + 1;
  }
  return false;
}

}  // namespace

// ===================== engine =====================

class BleService::Link final : public ESPRack::IBleLink {
 public:
  BleService* svc = nullptr;
  uint8_t index = 0;

  // ---- guarded by Radio::mx ----
  bool used = false;
  bool releaseReq = false;
  bool dirty = false;  // target / enable changed: drop and start over
  BleLinkSpec spec;    // copied at registration (callbacks never change afterwards)
  char peer[ESPRack::kBleAddrLen] = "";  // configured or discovered
  uint8_t peerType = 0;
  bool peerDiscovered = false;
  uint8_t peerFailures = 0;
  // Discovery: a candidate from the scan callback, and the scan generation
  // the link started waiting at.
  char candidate[ESPRack::kBleAddrLen] = "";
  uint8_t candidateType = 0;
  uint32_t waitScanGen = 0;
  uint32_t nextTryMs = 0;
  uint32_t backoffMs = 0;
  uint32_t lastRssiMs = 0;
  int8_t rssi_ = 0;
  uint16_t mtu_ = 0;
  BleLinkStats stats_;

  std::atomic<BleLinkState> st{BleLinkState::Disabled};
  BleLinkState notified = BleLinkState::Disabled;  // last state passed to onState (ble task)
  std::atomic<bool> dropped{false};  // set by the host's disconnect callback
  std::atomic<bool> notifyOn{false}; // onNotify may run (off once released)
  std::atomic<uint32_t> rxFrames{0}, rxBytes{0};

#if ESPRACK_HAVE_NIMBLE
  // ---- ble task only (writer pointer also under writeMx) ----
  NimBLEClient* client = nullptr;
  NimBLERemoteCharacteristic* writer = nullptr;
  struct Callbacks : public NimBLEClientCallbacks {
    Link* link = nullptr;
    void onDisconnect(NimBLEClient*, int reason) override {
      link->dropped = true;
      link->svc->wake();
      (void)reason;
    }
  } callbacks;
#endif
  SemaphoreHandle_t writeMx = nullptr;

  // ---- IBleLink ----
  bool write(const uint8_t* data, size_t len) override;
  BleLinkState state() const override { return st.load(); }
  void peerAddress(char* out, size_t len) const override;
  int8_t rssi() const override;
  uint16_t mtu() const override;
  BleLinkStats stats() const override;
  void setTarget(const char* address, uint8_t addressType) override;
  void setEnabled(bool enabled) override;
  void reconnect() override;
  void release() override;
};

struct BleService::Radio {
  SemaphoreHandle_t mx = nullptr;  // links, scan table, flags
  TaskHandle_t task = nullptr;
  Link links[kMaxLinks];

  // Settings applied by the task (copied under mx).
  bool wantEnabled = false;
  String name;
  int8_t txPower = 9;
  uint16_t mtu = 247;
  uint16_t scanIntervalMs = 100, scanWindowMs = 50;
  bool activeScan = true;
  uint32_t scanMs = 5000;
  bool hostParamsDirty = false;  // name / power / MTU changed: applied live
  bool scanParamsDirty = false;
  bool shuttingDown = false;
  std::atomic<bool> shutdownDone{false};  // the task has stopped the scan and the links
  SemaphoreHandle_t shutdownSem = nullptr;  // given with shutdownDone: end() waits on it

  // Host state (written by the task).
  std::atomic<bool> hostUp{false};
  char ownAddr[ESPRack::kBleAddrLen] = "";
  char hostError[40] = "";

  // Scan.
  uint32_t userScanReqMs = 0;  // pending request (0 = none)
  std::atomic<bool> scanning{false};
  std::atomic<uint32_t> scanGen{0};
  BleScanRow rows[ESPRACK_BLE_SCAN_ROWS];
  uint8_t rowCount = 0;

  void lock() { xSemaphoreTakeRecursive(mx, portMAX_DELAY); }
  void unlock() { xSemaphoreGiveRecursive(mx); }

#if ESPRACK_HAVE_NIMBLE
  struct ScanCallbacks : public NimBLEScanCallbacks {
    Radio* radio = nullptr;
    BleService* svc = nullptr;
    void onResult(const NimBLEAdvertisedDevice* dev) override { radio->onResult(dev); }
    void onScanEnd(const NimBLEScanResults&, int) override {
      radio->scanning = false;
      radio->scanGen.store(radio->scanGen.load() + 1);  // one writer; no RMW (Radio is on the heap)
      svc->wake();
    }
  } scanCb;

  void onResult(const NimBLEAdvertisedDevice* dev);
#endif
};

namespace {
class RadioLock {
 public:
  explicit RadioLock(BleService::Radio* r) : _r(r) { _r->lock(); }
  ~RadioLock() { _r->unlock(); }

 private:
  BleService::Radio* _r;
};
}  // namespace

// ---- IBleLink ----

bool BleService::Link::write(const uint8_t* data, size_t len) {
#if ESPRACK_HAVE_NIMBLE
  if (st.load() != BleLinkState::Connected || !data || !len) return false;
  xSemaphoreTake(writeMx, portMAX_DELAY);
  bool ok = false;
  if (writer && st.load() == BleLinkState::Connected) ok = writer->writeValue(data, len, spec.writeWithResponse);
  xSemaphoreGive(writeMx);
  RadioLock lock(svc->_radio);
  if (ok) {
    stats_.txFrames++;
    stats_.txBytes += len;
  } else {
    stats_.txErrors++;
  }
  return ok;
#else
  (void)data;
  (void)len;
  return false;
#endif
}

void BleService::Link::peerAddress(char* out, size_t len) const {
  RadioLock lock(svc->_radio);
  copyStr(out, peer, len);
}

int8_t BleService::Link::rssi() const {
  RadioLock lock(svc->_radio);
  return rssi_;
}

uint16_t BleService::Link::mtu() const {
  RadioLock lock(svc->_radio);
  return st.load() == BleLinkState::Connected ? mtu_ : 0;
}

BleLinkStats BleService::Link::stats() const {
  RadioLock lock(svc->_radio);
  BleLinkStats s = stats_;
  s.rxFrames = rxFrames.load();
  s.rxBytes = rxBytes.load();
  return s;
}

void BleService::Link::setTarget(const char* address, uint8_t addressType) {
  {
    RadioLock lock(svc->_radio);
    if (!strcasecmp(spec.address, address ? address : "") && spec.addressType == addressType) return;
    spec.setAddress(address, addressType);
    dirty = true;
  }
  svc->wake();
}

void BleService::Link::setEnabled(bool enabled) {
  {
    RadioLock lock(svc->_radio);
    if (spec.enabled == enabled) return;
    spec.enabled = enabled;
    dirty = true;
  }
  svc->wake();
}

void BleService::Link::reconnect() {
  {
    RadioLock lock(svc->_radio);
    dirty = true;
  }
  svc->wake();
}

void BleService::Link::release() {
  {
    RadioLock lock(svc->_radio);
    releaseReq = true;
  }
  svc->wake();
}

#if ESPRACK_HAVE_NIMBLE
// Host task: one advertisement (with scan response when active).
void BleService::Radio::onResult(const NimBLEAdvertisedDevice* dev) {
  const std::string addr = dev->getAddress().toString();
  const std::string name = dev->getName();
  std::string svcUuid;
  if (dev->haveServiceUUID()) svcUuid = dev->getServiceUUID(0).toString();
  uint16_t mfg = 0xFFFF;
  if (dev->getManufacturerDataCount() > 0) {
    const std::string m = dev->getManufacturerData(0);
    if (m.size() >= 2) mfg = uint16_t(uint8_t(m[0])) | uint16_t(uint8_t(m[1])) << 8;
  }

  RadioLock lock(this);
  // Upsert into the table; when full, replace the weakest row.
  int slot = -1;
  for (uint8_t i = 0; i < rowCount; i++) {
    if (!strcasecmp(rows[i].address, addr.c_str())) {
      slot = i;
      break;
    }
  }
  bool fresh = false;
  if (slot < 0) {
    fresh = true;
    if (rowCount < ESPRACK_BLE_SCAN_ROWS) {
      slot = rowCount++;
    } else {
      slot = 0;
      for (uint8_t i = 1; i < rowCount; i++)
        if (rows[i].rssi < rows[slot].rssi) slot = i;
      if (dev->getRSSI() <= rows[slot].rssi) slot = -1;
    }
  }
  if (slot >= 0) {
    BleScanRow& r = rows[slot];
    if (fresh) memset(&r, 0, sizeof(r));
    // A scan response may come without the name or the service: keep what we have.
    if (!name.empty()) copyStr(r.name, name.c_str(), sizeof(r.name));
    if (!svcUuid.empty()) copyStr(r.service, svcUuid.c_str(), sizeof(r.service));
    copyStr(r.address, addr.c_str(), sizeof(r.address));
    r.addressType = dev->getAddressType();
    r.rssi = dev->getRSSI();
    r.connectable = dev->isConnectable();
    if (mfg != 0xFFFF || fresh) r.mfgId = mfg;
    r.lastSeenMs = millis();
  }

  // Discovery: the first connectable match per waiting link.
  if (!dev->isConnectable()) return;
  for (Link& l : links) {
    if (!l.used || l.st.load() != BleLinkState::Discovering || l.candidate[0]) continue;
    bool match = prefixMatch(l.spec.namePrefixes, name.c_str());
    if (!match && l.spec.discoverService[0])
      match = dev->isAdvertisingService(NimBLEUUID(std::string(l.spec.discoverService)));
    if (!match) continue;
    // Not a peer another link already uses.
    bool taken = false;
    for (Link& o : links)
      if (&o != &l && o.used && o.peer[0] && !strcasecmp(o.peer, addr.c_str())) taken = true;
    if (taken) continue;
    copyStr(l.candidate, addr.c_str(), sizeof(l.candidate));
    l.candidateType = dev->getAddressType();
  }
}
#endif

// ===================== state <-> JSON =====================

void BleState::readConfig(BleState& s, JsonObject& root) {
  root["enabled"] = s.enabled;
  root["device_name"] = s.deviceName;
  root["tx_power"] = s.txPower;
  root["mtu"] = s.mtu;
  root["scan_interval_ms"] = s.scanIntervalMs;
  root["scan_window_ms"] = s.scanWindowMs;
  root["active_scan"] = s.activeScan;
  root["scan_s"] = s.scanSeconds;
}

StateUpdateResult BleState::update(JsonObject& root, BleState& s) {
  JsonObject src = root;
  if (root.containsKey("settings") && root["settings"].is<JsonObject>()) src = root["settings"].as<JsonObject>();

  bool changed = false;
  changed |= FormBuilder::updateValue(src, "enabled", s.enabled);
  if (src.containsKey("device_name") && src["device_name"].is<const char*>()) {
    String name = src["device_name"].as<const char*>();
    name.trim();
    if (name.length() > 29) name = name.substring(0, 29);  // fits the advertising packet
    if (name != s.deviceName) {
      s.deviceName = name;
      changed = true;
    }
  }
  int txPower = s.txPower;
  int mtu = s.mtu;
  int interval = s.scanIntervalMs;
  int window = s.scanWindowMs;
  int scanS = s.scanSeconds;
  changed |= FormBuilder::updateValue(src, "tx_power", txPower);
  changed |= FormBuilder::updateValue(src, "mtu", mtu);
  changed |= FormBuilder::updateValue(src, "scan_interval_ms", interval);
  changed |= FormBuilder::updateValue(src, "scan_window_ms", window);
  changed |= FormBuilder::updateValue(src, "active_scan", s.activeScan);
  changed |= FormBuilder::updateValue(src, "scan_s", scanS);

  // Clamp: REST and WS callers bypass the UI's limits.
  s.txPower = int8_t(constrain(txPower, -24, 20));
  s.mtu = uint16_t(constrain(mtu, 23, 517));
  s.scanIntervalMs = uint16_t(constrain(interval, 10, 10000));
  s.scanWindowMs = uint16_t(constrain(window, 10, (int)s.scanIntervalMs));
  s.scanSeconds = uint8_t(constrain(scanS, 1, 60));
  return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
}

namespace {
const char* radioText(const BleState& s) {
  if (!s.compiledIn) return "Stack not in this build";
  if (s.safeMode) return "Off (safe mode)";
  if (!s.enabled) return "Disabled";
  if (!s.hostUp) return s.hostError[0] ? "Error" : "Starting";
  return "Running";
}

void fillLinkRows(const BleState& s, JsonArray rows) {
  for (const BleLinkRow& l : s.links) {
    if (!l.used) continue;
    JsonObject r = rows.createNestedObject();
    r["owner"] = (char*)l.owner;
    r["peer"] = (char*)(l.peer[0] ? l.peer : "—");
    r["state"] = ESPRack::bleLinkStateName(l.state);
    r["rssi"] = l.rssi;
    r["mtu"] = l.mtu;
    r["rx"] = l.stats.rxFrames;
    r["tx"] = l.stats.txFrames;
    r["connects"] = l.stats.connects;
    r["drops"] = l.stats.drops;
    r["error"] = (char*)l.stats.lastError;
  }
}
}  // namespace

void BleState::buildForm(BleState& s, JsonObject& root) {
  // ---- Status (live) ----
  web::Form st(root, "status", "Bluetooth LE");
  if (!s.compiledIn) {
    st.add(web::Message("m_stack",
                        "This firmware has no BLE stack: add h2zero/NimBLE-Arduino @ 2.5.1 to lib_deps "
                        "(the chip needs a BLE radio).")
               .level("warning"));
  } else if (s.safeMode) {
    st.add(web::Message("m_safe", "BootGuard safe mode: the radio stays off until safe mode is cleared.")
               .level("warning"));
  }
  st.add(web::Text("radio", radioText(s)).readOnly().label("Radio")
             .colorMap("Running:success,Starting:info,Disabled:info,default:warning").icon("Bluetooth"));
  st.add(web::Text("own_addr", s.ownAddr[0] ? s.ownAddr : "—").readOnly().label("Address").icon("Fingerprint"));
  st.add(web::Text("host_error", s.hostError[0] ? s.hostError : "—").readOnly().label("Last host error")
             .icon("ReportProblem"));
  char links[16];
  snprintf(links, sizeof(links), "%u / %u", s.linkCount, s.maxLinks);
  st.add(web::Text("links", links).readOnly().label("Links").icon("Link"));
  st.add(web::Table("links_table")
             .col("owner", "Used by", "text")
             .col("peer", "Peer", "text")
             .col("state", "State", "text")
             .col("rssi", "RSSI, dBm", "number", "0")
             .col("mtu", "MTU", "number", "0")
             .col("rx", "RX", "number", "0")
             .col("tx", "TX", "number", "0")
             .col("connects", "Connects", "number", "0")
             .col("drops", "Drops", "number", "0")
             .col("error", "Last error", "text")
             .mode("replace").label("Central links").icon("Link"));
  fillLinkRows(s, st.rowsOf("links_table"));

  // ---- Scan (REST: rows baked into the GET, refetched after ble.scan) ----
  web::Form sc(root, "scan", "Nearby devices");
  sc.add(web::Action("a_scan").ref("ble.scan").refetchForm().loadingIf("scan_state:Scanning"));
  sc.add(web::Text("scan_state", s.scanning ? "Scanning…" : (s.scanGen ? "Done" : "—")).readOnly()
             .label("Scan").icon("Radar"));
  sc.add(web::Table("scan")
             .col("name", "Name", "text")
             .col("address", "Address", "text")
             .col("rssi", "RSSI, dBm", "number", "0")
             .col("type", "Type", "text")
             .col("connectable", "Connectable", "boolean")
             .col("service", "Service", "text")
             .col("mfg", "Mfg id", "text")
             .col("age", "Seen, s ago", "number", "0")
             .mode("replace").loadingIf("scan_state:Scanning").label("Scan results").icon("Bluetooth"));
  JsonArray rows = sc.rowsOf("scan");
  const uint32_t now = millis();
  for (uint8_t i = 0; i < s.scanCount; i++) {
    const BleScanRow& r = s.scan[i];
    JsonObject o = rows.createNestedObject();
    o["name"] = (char*)(r.name[0] ? r.name : "");
    o["address"] = (char*)r.address;
    o["rssi"] = r.rssi;
    o["type"] = r.addressType ? "random" : "public";
    o["connectable"] = r.connectable;
    o["service"] = (char*)r.service;
    if (r.mfgId != 0xFFFF) {
      char mfg[8];
      snprintf(mfg, sizeof(mfg), "0x%04X", r.mfgId);
      o["mfg"] = String(mfg);
    } else {
      o["mfg"] = "";
    }
    o["age"] = (now - r.lastSeenMs) / 1000;
  }

  // ---- Settings (admin) ----
  web::Form set(root, "settings", "Radio settings");
  set.add(web::Switch("enabled", s.enabled).label("Bluetooth enabled").icon("Bluetooth"));
  set.add(web::Text("device_name", s.deviceName.c_str()).label("Device name (empty = device id)")
              .placeholder("MyDevice-1a2b3c4d").icon("Badge"));
  set.add(web::Number("tx_power", s.txPower).min(-24).max(20).format("0").unit("dBm").label("TX power")
              .icon("SignalCellularAlt"));
  set.add(web::Number("mtu", s.mtu).min(23).max(517).format("0").label("Preferred ATT MTU").icon("Straighten"));
  set.add(web::Number("scan_interval_ms", s.scanIntervalMs).min(10).max(10000).format("0").unit("ms")
              .label("Scan interval").icon("Timer"));
  set.add(web::Number("scan_window_ms", s.scanWindowMs).min(10).max(10000).format("0").unit("ms")
              .label("Scan window (≤ interval; less leaves more air time to Wi-Fi)").icon("Timer"));
  set.add(web::Switch("active_scan", s.activeScan).label("Active scan (ask for scan responses)").icon("Radar"));
  set.add(web::Number("scan_s", s.scanSeconds).min(1).max(60).format("0").unit("s").label("Scan duration")
              .icon("HourglassBottom"));
  set.add(web::Message("m_apply", "Changes apply live: TX power at once, the MTU to the next connections, "
                                  "the name to the GAP service.")
              .level("info"));
}

void BleState::readState(BleState& s, JsonObject& root) {
  root["radio"] = radioText(s);
  root["own_addr"] = (char*)(s.ownAddr[0] ? s.ownAddr : "—");
  root["host_error"] = (char*)(s.hostError[0] ? s.hostError : "—");
  char links[16];
  snprintf(links, sizeof(links), "%u / %u", s.linkCount, s.maxLinks);
  root["links"] = String(links);
  fillLinkRows(s, root.createNestedArray("links_table"));
  root["scan_state"] = s.scanning ? "Scanning…" : (s.scanGen ? "Done" : "—");
  root["scan_count"] = s.scanCount;
  root["enabled"] = s.enabled;
  root["tx_power"] = s.txPower;
  root["mtu"] = s.mtu;
}

// ===================== service =====================

BleService::BleService(ConfigManager* cfgMgr)
    : StatefulService<BleState>(),
      _cfg(cfgMgr, "ble", BLE_CONFIG_FILE, 1024, this, BleState::readConfig, BleState::update, false, nullptr,
           BleState::buildForm) {
  _radio = new Radio();
  _radio->mx = xSemaphoreCreateRecursiveMutex();
  _radio->shutdownSem = xSemaphoreCreateBinary();
  for (uint8_t i = 0; i < kMaxLinks; i++) {
    Link& l = _radio->links[i];
    l.svc = this;
    l.index = i;
    l.writeMx = xSemaphoreCreateMutex();
#if ESPRACK_HAVE_NIMBLE
    l.callbacks.link = &l;
#endif
  }
#if ESPRACK_HAVE_NIMBLE
  _radio->scanCb.radio = _radio;
  _radio->scanCb.svc = this;
#endif

  updateWithoutPropagation([](BleState& s) {
    s.compiledIn = ESPRACK_HAVE_NIMBLE;
    s.maxLinks = kMaxLinks;
    return StateUpdateResult::CHANGED;
  });

  // UI save, POST /state, WS write: persist, then the task applies it.
  addUpdateHandler([this](const String& origin) {
    _cfg.saveIfChanged(origin);
    _applyPending = true;
    ESPRack::wakeLoop();  // loop() hands them to the task
  }, false);
}

void BleService::registerManifest(WebManager* web) {
  if (!web) return;
  _feature = web->registerFeature<BleState>(
      WebFeature("ble")
          .title("Bluetooth")
          .version(BLE_MODULE_VERSION)
          .component(WebComponent::DynamicSettings)
          .authenticated()
          .adminWrite()
          .menu(WebMenu("Bluetooth").icon("Bluetooth").order(145))
          .ws("/ws/ble")
          .tab(WebTab("status", "Status").icon("Monitor").live())
          .tab(WebTab("scan", "Scan").icon("Radar"))
          .tab(WebTab("settings", "Settings").icon("Tune").postable().adminOnly())
          .build(),
      this,
      BleState::buildForm, BleState::update,
      BleState::readState, BleState::update,
      // The scan table is baked into GET /form: 24 rows of ~170 B plus the
      // rest of the schema. The WS frame carries the links table only.
      7168, 2048);

  web->registerAction(
      WebAction("ble.scan", "Scan")
          .icon("Radar").color("primary")
          .authenticated()
          .success("Scanning…")
          .handler([this](AsyncWebServerRequest* r) {
            if (!requestScan(0)) {
              r->send(409, "application/json", "{\"error\":\"radio_off\"}");
              return;
            }
            r->send(200, "application/json", "{\"ok\":true}");
          }));
}

void BleService::begin() {
  (void)_cfg.ensureLoaded();
  const bool safe = ESPRack::BootGuard::info().safeMode;
  {
    RadioLock lock(_radio);
    read([&](BleState& s) {
      _radio->wantEnabled = s.enabled && !safe;
      _radio->name = s.deviceName;
      _radio->txPower = s.txPower;
      _radio->mtu = s.mtu;
      _radio->scanIntervalMs = s.scanIntervalMs;
      _radio->scanWindowMs = s.scanWindowMs;
      _radio->activeScan = s.activeScan;
      _radio->scanMs = uint32_t(s.scanSeconds) * 1000;
    });
    if (!_radio->name.length())
      _radio->name = DeviceIdentity::projectName() + "-" + DeviceIdentity::uidHex8();
  }
  updateWithoutPropagation([safe](BleState& s) {
    s.safeMode = safe;
    return StateUpdateResult::CHANGED;
  });
#if ESPRACK_HAVE_NIMBLE
  if (!_radio->task) {
    _radio->task = xTaskCreateStaticPinnedToCore(taskEntry, "ble", ESPRACK_BLE_TASK_STACK, this, 2, s_stack, &s_tcb,
                                                 tskNO_AFFINITY);
  }
#endif
}

void BleService::loop() {
  // Load + store, not exchange (no RMW atomics on the heap object): a save
  // between the two is applied right here.
  if (_applyPending.load()) {
    _applyPending.store(false);
    // Settings saved: hand them to the task.
    const bool safe = ESPRack::BootGuard::info().safeMode;
    RadioLock lock(_radio);
    const bool got = read([&](BleState& s) {
      String name = s.deviceName.length() ? s.deviceName
                                          : DeviceIdentity::projectName() + "-" + DeviceIdentity::uidHex8();
      if (name != _radio->name || s.txPower != _radio->txPower || s.mtu != _radio->mtu)
        _radio->hostParamsDirty = true;
      if (s.scanIntervalMs != _radio->scanIntervalMs || s.scanWindowMs != _radio->scanWindowMs ||
          s.activeScan != _radio->activeScan)
        _radio->scanParamsDirty = true;
      _radio->wantEnabled = s.enabled && !safe;
      _radio->name = name;
      _radio->txPower = s.txPower;
      _radio->mtu = s.mtu;
      _radio->scanIntervalMs = s.scanIntervalMs;
      _radio->scanWindowMs = s.scanWindowMs;
      _radio->activeScan = s.activeScan;
      _radio->scanMs = uint32_t(s.scanSeconds) * 1000;
    });
    if (got) wake();
    else _applyPending.store(true);  // state lock timed out: the radio keeps its settings, the next pass
  }

  const uint32_t now = millis();
  if (now - _lastMirrorMs >= kMirrorMs) {
    _lastMirrorMs = now;
    mirror();
    if (_feature && _feature->hasSubscribers()) _feature->broadcastWs("tick");
  }
}

uint32_t BleService::loopWaitMs(uint32_t nowMs) const {
  if (_applyPending.load()) return 0;
  return ESPRack::LoopWait::dueIn(nowMs, _lastMirrorMs, kMirrorMs);
}

void BleService::end() {
#if ESPRACK_HAVE_NIMBLE
  // restartDevice(): the ble task owns every stack object, so it does the
  // teardown (stop the scan, drop the links); wait a bounded time for it.
  {
    RadioLock lock(_radio);
    _radio->shuttingDown = true;
  }
  wake();
  // The task gives shutdownSem once the scan and the links are down: wait
  // for that, 300 ms at most (within the onShutdown budget).
  if (_radio->task && !_radio->shutdownDone.load() && _radio->shutdownSem)
    xSemaphoreTake(_radio->shutdownSem, pdMS_TO_TICKS(300));
#endif
}

void BleService::wake() {
  if (_radio && _radio->task) xTaskNotifyGive(_radio->task);
}

// Engine -> BleState for the UI (loop task, once a second).
void BleService::mirror() {
  BleLinkRow rows[kMaxLinks];
  uint8_t count = 0;
  bool hostUp, scanningNow;
  char own[ESPRack::kBleAddrLen];
  char err[40];
  uint32_t gen;
  {
    RadioLock lock(_radio);
    for (uint8_t i = 0; i < kMaxLinks; i++) {
      const Link& l = _radio->links[i];
      rows[i].used = l.used;
      if (!l.used) continue;
      count++;
      copyStr(rows[i].owner, l.spec.owner, sizeof(rows[i].owner));
      copyStr(rows[i].peer, l.peer, sizeof(rows[i].peer));
      rows[i].state = l.st.load();
      rows[i].rssi = l.rssi_;
      rows[i].mtu = rows[i].state == BleLinkState::Connected ? l.mtu_ : 0;
      rows[i].stats = l.stats_;
      rows[i].stats.rxFrames = l.rxFrames.load();
      rows[i].stats.rxBytes = l.rxBytes.load();
    }
    hostUp = _radio->hostUp.load();
    scanningNow = _radio->scanning.load();
    copyStr(own, _radio->ownAddr, sizeof(own));
    copyStr(err, _radio->hostError, sizeof(err));
    gen = _radio->scanGen.load();
  }
  // The scan table only while it changes (it is the bulk of the copy).
  const bool copyScan = scanningNow || gen != _mirroredScanGen;
  _mirroredScanGen = gen;
  BleScanRow* scanRows = nullptr;
  uint8_t scanCount = 0;
  if (copyScan) {
    scanRows = new (std::nothrow) BleScanRow[ESPRACK_BLE_SCAN_ROWS];
    if (scanRows) scanCount = uint8_t(scanResults(scanRows, ESPRACK_BLE_SCAN_ROWS));
  }
  updateWithoutPropagation([&](BleState& s) {
    for (uint8_t i = 0; i < kMaxLinks && i < ESPRACK_BLE_MAX_LINKS; i++) s.links[i] = rows[i];
    s.linkCount = count;
    s.hostUp = hostUp;
    s.scanning = scanningNow;
    s.scanGen = gen;
    copyStr(s.ownAddr, own, sizeof(s.ownAddr));
    copyStr(s.hostError, err, sizeof(s.hostError));
    if (scanRows) {
      memcpy(s.scan, scanRows, sizeof(BleScanRow) * scanCount);
      s.scanCount = scanCount;
    }
    return StateUpdateResult::CHANGED;
  });
  delete[] scanRows;
}

// ---- IBleProvider ----

bool BleService::ready() const { return _radio && _radio->hostUp.load(); }

void BleService::ownAddress(char* out, size_t len) const {
  RadioLock lock(_radio);
  copyStr(out, _radio->ownAddr, len);
}

bool BleService::requestScan(uint32_t durationMs) {
  {
    RadioLock lock(_radio);
    if (!_radio->wantEnabled || _radio->shuttingDown) return false;
    _radio->userScanReqMs = durationMs ? durationMs : _radio->scanMs;
  }
  wake();
  return true;
}

bool BleService::scanning() const { return _radio && (_radio->scanning.load() || _radio->userScanReqMs); }

uint32_t BleService::scanGeneration() const { return _radio ? _radio->scanGen.load() : 0; }

size_t BleService::scanResults(BleScanRow* out, size_t max) const {
  if (!out || !max) return 0;
  RadioLock lock(_radio);
  const size_t n = _radio->rowCount < max ? _radio->rowCount : max;
  // Strongest first: copy, then a small insertion sort (n <= 24).
  BleScanRow tmp[ESPRACK_BLE_SCAN_ROWS];
  memcpy(tmp, _radio->rows, sizeof(BleScanRow) * _radio->rowCount);
  for (uint8_t i = 1; i < _radio->rowCount; i++) {
    BleScanRow key = tmp[i];
    int j = i - 1;
    while (j >= 0 && tmp[j].rssi < key.rssi) {
      tmp[j + 1] = tmp[j];
      j--;
    }
    tmp[j + 1] = key;
  }
  memcpy(out, tmp, sizeof(BleScanRow) * n);
  return n;
}

ESPRack::IBleLink* BleService::registerLink(const BleLinkSpec& spec, const char** why) {
#if !ESPRACK_HAVE_NIMBLE
  (void)spec;
  if (why) *why = "BLE stack not in this build (add NimBLE-Arduino to lib_deps)";
  return nullptr;
#else
  if (!spec.service[0]) {
    if (why) *why = "spec.service is empty";
    return nullptr;
  }
  if (!spec.address[0] && !spec.namePrefixes[0] && !spec.discoverService[0]) {
    if (why) *why = "no address, name prefix or service to find the peer by";
    return nullptr;
  }
  Link* slot = nullptr;
  {
    RadioLock lock(_radio);
    for (Link& l : _radio->links) {
      if (!l.used) {
        slot = &l;
        break;
      }
    }
    if (!slot) {
      if (why) *why = "no free BLE link slot (ESPRACK_BLE_MAX_LINKS)";
      return nullptr;
    }
    slot->used = true;
    slot->releaseReq = false;
    slot->dirty = false;
    slot->spec = spec;
    if (!slot->spec.writeChar[0]) slot->spec.setWriteChar(spec.notifyChar);
    copyStr(slot->peer, spec.address, sizeof(slot->peer));
    slot->peerType = spec.addressType;
    slot->peerDiscovered = false;
    slot->peerFailures = 0;
    slot->candidate[0] = '\0';
    slot->nextTryMs = millis();
    slot->backoffMs = spec.backoffMinMs;
    slot->rssi_ = 0;
    slot->mtu_ = 0;
    slot->stats_ = BleLinkStats{};
    slot->rxFrames = 0;
    slot->rxBytes = 0;
    slot->dropped = false;
    slot->st = spec.enabled ? BleLinkState::Backoff : BleLinkState::Disabled;
    slot->notified = BleLinkState::Disabled;
    slot->notifyOn = true;
  }
  log_i("[ble] link %u registered for %s", slot->index, spec.owner);
  wake();
  return slot;
#endif
}

uint8_t BleService::maxLinks() const { return kMaxLinks; }

uint8_t BleService::linkCount() const {
  RadioLock lock(_radio);
  uint8_t n = 0;
  for (const Link& l : _radio->links) n += l.used ? 1 : 0;
  return n;
}

// ===================== the ble task =====================

void BleService::taskEntry(void* self) { static_cast<BleService*>(self)->taskLoop(); }

#if !ESPRACK_HAVE_NIMBLE

void BleService::taskLoop() {
  vTaskDelete(nullptr);  // never started without NimBLE
}

#else

namespace {

// A primary service by UUID, from the full service list. Discovering "by UUID" uses ATT Find By
// Type Value, and some peripherals answer it with a wrong end handle (end == start): the service
// is found but holds no characteristics. Read By Group Type (all services) gets the real range.
NimBLERemoteService* findService(NimBLEClient* client, const char* uuid) {
  const NimBLEUUID want{std::string(uuid)};
  for (NimBLERemoteService* s : client->getServices(true))
    if (s && s->getUUID() == want) return s;
  return client->getService(want);
}

void logServices(const char* owner, NimBLEClient* client) {
  const auto& services = client->getServices(false);
  log_w("[ble] %s: peer has %u service(s):", owner, (unsigned)services.size());
  for (NimBLERemoteService* s : services) {
    if (!s) continue;
    log_w("[ble]   %s handles 0x%04x-0x%04x", s->getUUID().toString().c_str(), s->getStartHandle(),
          s->getEndHandle());
  }
}

// A characteristic of `service` by UUID. NimBLE asks the peer to "discover characteristics by
// UUID" first; some peripherals (cheap BLE bridge chips among them)
// answer that request wrongly, so fall back to listing all of them and matching here.
NimBLERemoteCharacteristic* findCharacteristic(NimBLERemoteService* service, const char* uuid) {
  const NimBLEUUID want{std::string(uuid)};
  if (NimBLERemoteCharacteristic* c = service->getCharacteristic(want)) return c;
  for (NimBLERemoteCharacteristic* c : service->getCharacteristics(true))
    if (c && c->getUUID() == want) return c;
  return nullptr;
}

// What the peer really offers, for the log when a lookup fails.
void logCharacteristics(const char* owner, NimBLERemoteService* service) {
  const auto& chars = service->getCharacteristics(false);
  log_w("[ble] %s: service %s (handles 0x%04x-0x%04x) has %u characteristic(s):", owner,
        service->getUUID().toString().c_str(), service->getStartHandle(), service->getEndHandle(),
        (unsigned)chars.size());
  for (NimBLERemoteCharacteristic* c : chars) {
    if (!c) continue;
    log_w("[ble]   %s %s%s%s%s%s", c->getUUID().toString().c_str(), c->canRead() ? "R" : "-",
          c->canWrite() ? "W" : "-", c->canWriteNoResponse() ? "w" : "-", c->canNotify() ? "N" : "-",
          c->canIndicate() ? "I" : "-");
  }
}

// The state only; onState runs later from notifyStates(), outside every lock,
// so a consumer callback can never deadlock against the radio mutex.
void setState(BleService::Link& l, BleLinkState s) { l.st.store(s); }

}  // namespace

void BleService::notifyStates() {
  for (Link& l : _radio->links) {
    std::function<void(BleLinkState)> cb;
    BleLinkState now;
    {
      RadioLock lock(_radio);
      if (!l.used) continue;
      now = l.st.load();
      if (now == l.notified) continue;
      l.notified = now;
      cb = l.spec.onState;
    }
    if (cb) cb(now);
  }
}

void BleService::taskLoop() {
  Radio& r = *_radio;
  for (;;) {
    // ---- settings snapshot ----
    bool want, hostDirty, scanDirty, shutting;
    String name;
    int8_t power;
    uint16_t mtu, itvl, win;
    bool active;
    uint32_t userScan;
    {
      RadioLock lock(&r);
      want = r.wantEnabled && !r.shuttingDown;
      hostDirty = r.hostParamsDirty;
      r.hostParamsDirty = false;
      scanDirty = r.scanParamsDirty;
      r.scanParamsDirty = false;
      shutting = r.shuttingDown;
      name = r.name;
      power = r.txPower;
      mtu = r.mtu;
      itvl = r.scanIntervalMs;
      win = r.scanWindowMs;
      active = r.activeScan;
      userScan = r.userScanReqMs;
      r.userScanReqMs = 0;
    }

    // ---- host lifecycle ----
    const bool up = r.hostUp.load();
    if (up && shutting) {
      // restartDevice(): stop scanning and drop the links, then park. No deinit:
      // the chip restarts right after the shutdown hooks.
      NimBLEDevice::getScan()->stop();
      for (Link& l : r.links) {
        xSemaphoreTake(l.writeMx, portMAX_DELAY);
        l.writer = nullptr;
        xSemaphoreGive(l.writeMx);
        if (l.client && l.client->isConnected()) l.client->disconnect();
      }
      r.shutdownDone = true;
      if (r.shutdownSem) xSemaphoreGive(r.shutdownSem);
      // Parked until the restart (own task; nothing else is left to do).
      for (;;) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    if (up && !want) {
      // Radio off. Disconnect each client and wait for it (NimBLE refuses to
      // delete a client that is still disconnecting, and deinit would leak it),
      // delete it, and only then stop the host.
      NimBLEDevice::getScan()->stop();
      for (Link& l : r.links) {
        xSemaphoreTake(l.writeMx, portMAX_DELAY);
        l.writer = nullptr;
        xSemaphoreGive(l.writeMx);
        if (!l.client) continue;
        if (l.client->isConnected()) {
          l.client->disconnect();
          // onDisconnect wakes the task: wait for it, 2 s at most.
          const uint32_t t0 = millis();
          while (l.client->isConnected() && (uint32_t)(millis() - t0) < 2000)
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000 - (millis() - t0)) + 1);
        }
        if (!NimBLEDevice::deleteClient(l.client)) log_w("[ble] %s: client not deleted", l.spec.owner);
        l.client = nullptr;
      }
      NimBLEDevice::deinit(true);
      {
        RadioLock lock(&r);
        for (Link& l : r.links) {
          l.client = nullptr;
          l.dropped = false;
          if (l.used) {
            setState(l, BleLinkState::Disabled);
            l.nextTryMs = millis();
          }
        }
        r.ownAddr[0] = '\0';
        r.scanning = false;
      }
      r.hostUp = false;
      log_i("[ble] host stopped");
    }
    if (!r.hostUp.load() && want) {
      if (NimBLEDevice::init(std::string(name.c_str()))) {
        NimBLEDevice::setPower(power);
        NimBLEDevice::setMTU(mtu);
        NimBLEScan* scan = NimBLEDevice::getScan();
        scan->setScanCallbacks(&r.scanCb, false);
        scan->setMaxResults(0);  // our own table only
        scan->setActiveScan(active);
        scan->setInterval(itvl);
        scan->setWindow(win);
        RadioLock lock(&r);
        copyStr(r.ownAddr, NimBLEDevice::getAddress().toString().c_str(), sizeof(r.ownAddr));
        r.hostError[0] = '\0';
        r.hostUp = true;
        for (Link& l : r.links)
          if (l.used && l.spec.enabled) setState(l, BleLinkState::Backoff);
        log_i("[ble] host up: %s (%s)", r.ownAddr, name.c_str());
      } else {
        RadioLock lock(&r);
        copyStr(r.hostError, "NimBLEDevice::init failed", sizeof(r.hostError));
      }
    }
    if (!r.hostUp.load()) {
      if (shutting) {
        r.shutdownDone = true;
        if (r.shutdownSem) xSemaphoreGive(r.shutdownSem);
      }
      notifyStates();
      // Host off: re-check once a second; shutting down: parked until the restart.
      ulTaskNotifyTake(pdTRUE, shutting ? portMAX_DELAY : pdMS_TO_TICKS(1000));
      continue;
    }
    if (hostDirty) {
      // Live: TX power now, MTU for the next connections, the name for the GAP service.
      NimBLEDevice::setPower(power);
      NimBLEDevice::setMTU(mtu);
      NimBLEDevice::setDeviceName(std::string(name.c_str()));
    }
    if (scanDirty) {
      NimBLEScan* scan = NimBLEDevice::getScan();
      scan->setActiveScan(active);
      scan->setInterval(itvl);
      scan->setWindow(win);
    }

    // ---- scans: a user request, or a link waiting for discovery ----
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (userScan && !shutting) {
      {
        RadioLock lock(&r);
        r.rowCount = 0;  // a fresh table for the operator
      }
      if (r.scanning.load()) scan->stop();
      r.scanning = scan->start(userScan, false, true);
    }
    // A link entering Discovering records the scan generation; it asks for a scan
    // only until one has ended since then, so a failed discovery goes to Backoff
    // instead of scanning back to back.
    uint32_t discoverMs = 0;
    {
      RadioLock lock(&r);
      const uint32_t gen = r.scanGen.load();
      for (Link& l : r.links)
        if (l.used && l.st.load() == BleLinkState::Discovering && !l.candidate[0] && l.waitScanGen == gen &&
            l.spec.discoverScanMs > discoverMs)
          discoverMs = l.spec.discoverScanMs;
    }
    if (discoverMs && !r.scanning.load() && !shutting) {
      r.scanning = scan->start(discoverMs, false, true);
      if (!r.scanning.load()) r.scanGen++;  // refused: count it as an empty scan, back off
    }

    // ---- links ----
    for (Link& l : r.links) {
      if (shutting) break;
      const uint32_t now = millis();

      // Snapshot of what the task needs, under the lock.
      bool used, release, dirty, enabled;
      BleLinkState st;
      {
        RadioLock lock(&r);
        used = l.used;
        release = l.releaseReq;
        dirty = l.dirty;
        l.dirty = false;
        enabled = l.spec.enabled;
        st = l.st.load();
      }
      if (!used) continue;

      auto disconnect = [&]() {
        xSemaphoreTake(l.writeMx, portMAX_DELAY);
        l.writer = nullptr;
        xSemaphoreGive(l.writeMx);
        if (l.client && l.client->isConnected()) l.client->disconnect();
      };
      auto fail = [&](const char* why) {
        disconnect();
        RadioLock lock(&r);
        l.stats_.failures++;
        copyStr(l.stats_.lastError, why, sizeof(l.stats_.lastError));
        if (l.peerDiscovered && ++l.peerFailures >= kForgetDiscoveredAfter) {
          l.peer[0] = '\0';  // maybe another device now: find it again
          l.peerDiscovered = false;
          l.peerFailures = 0;
        }
        l.nextTryMs = millis() + l.backoffMs;
        l.backoffMs = l.backoffMs * 2 > l.spec.backoffMaxMs ? l.spec.backoffMaxMs : l.backoffMs * 2;
        setState(l, BleLinkState::Backoff);
        log_w("[ble] %s: %s", l.spec.owner, why);
      };

      if (release) {
        l.notifyOn = false;  // a notification still in flight is dropped
        disconnect();
        if (l.client) {
          NimBLEDevice::deleteClient(l.client);
          l.client = nullptr;
        }
        RadioLock lock(&r);
        l.used = false;
        l.releaseReq = false;
        l.st = BleLinkState::Disabled;
        l.notified = BleLinkState::Disabled;
        continue;
      }
      if (dirty) {
        disconnect();
        RadioLock lock(&r);
        copyStr(l.peer, l.spec.address, sizeof(l.peer));
        l.peerType = l.spec.addressType;
        l.peerDiscovered = false;
        l.peerFailures = 0;
        l.candidate[0] = '\0';
        l.backoffMs = l.spec.backoffMinMs;
        l.nextTryMs = now;
        l.dropped = false;
        setState(l, enabled ? BleLinkState::Backoff : BleLinkState::Disabled);
        st = l.st.load();
      }
      if (!enabled) {
        if (st != BleLinkState::Disabled) {
          disconnect();
          setState(l, BleLinkState::Disabled);
        }
        continue;
      }

      switch (st) {
        case BleLinkState::Disabled: {
          RadioLock lock(&r);
          l.nextTryMs = now;
          setState(l, BleLinkState::Backoff);
          break;
        }

        case BleLinkState::Backoff: {
          bool go, havePeer;
          {
            RadioLock lock(&r);
            go = (int32_t)(now - l.nextTryMs) >= 0;
            havePeer = l.peer[0] != '\0';
            if (go && !havePeer) {
              l.candidate[0] = '\0';
              l.waitScanGen = r.scanGen.load();
              setState(l, BleLinkState::Discovering);
            }
          }
          if (!go || !havePeer) break;
          // Connecting while scanning is refused by the controller: wait for the scan.
          if (r.scanning.load()) break;
          setState(l, BleLinkState::Connecting);
          // fall through to the attempt below
          char peer[ESPRack::kBleAddrLen];
          uint8_t type;
          {
            RadioLock lock(&r);
            copyStr(peer, l.peer, sizeof(peer));
            type = l.peerType;
            l.dropped = false;
          }
          if (!l.client) {
            l.client = NimBLEDevice::createClient();
            if (!l.client) {
              fail("no client (stack out of connections)");
              break;
            }
            l.client->setClientCallbacks(&l.callbacks, false);
          }
          l.client->setConnectTimeout(l.spec.connectTimeoutMs);
          if (!l.client->connect(NimBLEAddress(std::string(peer), type), true, false, true)) {
            fail("connect failed");
            break;
          }
          // Let the peer settle (connection parameter update, its own GATT setup) before
          // discovery: 200 ms as a wait on the task's notification (a wake meanwhile is
          // seen on the next pass anyway), cut short only by a shutdown.
          for (const uint32_t t0 = millis(); (uint32_t)(millis() - t0) < 200 && !r.shuttingDown;)
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200 - (millis() - t0)) + 1);
          NimBLERemoteService* service = findService(l.client, l.spec.service);
          if (!service) {
            logServices(l.spec.owner, l.client);
            fail("service not found");
            break;
          }
          NimBLERemoteCharacteristic* notify = nullptr;
          if (l.spec.notifyChar[0]) {
            notify = findCharacteristic(service, l.spec.notifyChar);
            if (!notify) {
              logServices(l.spec.owner, l.client);
              logCharacteristics(l.spec.owner, service);
              fail("notify characteristic not found");
              break;
            }
            if (!(notify->canNotify() || notify->canIndicate())) {
              logCharacteristics(l.spec.owner, service);
              fail("characteristic cannot notify");
              break;
            }
          }
          NimBLERemoteCharacteristic* writer = nullptr;
          if (l.spec.writeChar[0]) {
            writer = findCharacteristic(service, l.spec.writeChar);
            if (!writer || !(writer->canWrite() || writer->canWriteNoResponse())) {
              logCharacteristics(l.spec.owner, service);
              fail("write characteristic not found");
              break;
            }
          }
          if (notify) {
            Link* self = &l;
            const bool ok = notify->subscribe(
                notify->canNotify(),
                [self](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
                  if (!self->notifyOn.load()) return;
                  self->rxFrames++;
                  self->rxBytes += len;
                  if (self->spec.onNotify) self->spec.onNotify(data, len);
                },
                true);
            if (!ok) {
              fail("subscribe failed");
              break;
            }
          }
          xSemaphoreTake(l.writeMx, portMAX_DELAY);
          l.writer = writer;
          xSemaphoreGive(l.writeMx);
          {
            RadioLock lock(&r);
            l.mtu_ = l.client->getMTU();
            l.rssi_ = int8_t(l.client->getRssi());
            l.lastRssiMs = millis();
            l.stats_.connects++;
            l.stats_.connectedSinceMs = millis() | 1;
            l.stats_.lastError[0] = '\0';
            l.backoffMs = l.spec.backoffMinMs;
            l.peerFailures = 0;
          }
          setState(l, BleLinkState::Connected);
          log_i("[ble] %s connected to %s (MTU %u)", l.spec.owner, peer, l.mtu_);
          break;
        }

        case BleLinkState::Discovering: {
          RadioLock lock(&r);
          if (l.candidate[0]) {
            copyStr(l.peer, l.candidate, sizeof(l.peer));
            l.peerType = l.candidateType;
            l.peerDiscovered = true;
            l.peerFailures = 0;
            l.candidate[0] = '\0';
            l.nextTryMs = now;  // connect on the next pass (after the scan ends)
            setState(l, BleLinkState::Backoff);
            log_i("[ble] %s found %s", l.spec.owner, l.peer);
          } else if (!r.scanning.load() && r.scanGen.load() != l.waitScanGen) {
            // A scan ended since we started waiting, nothing matched.
            l.stats_.failures++;
            copyStr(l.stats_.lastError, "no matching device found", sizeof(l.stats_.lastError));
            l.nextTryMs = now + l.backoffMs;
            l.backoffMs = l.backoffMs * 2 > l.spec.backoffMaxMs ? l.spec.backoffMaxMs : l.backoffMs * 2;
            setState(l, BleLinkState::Backoff);
          }
          break;
        }

        case BleLinkState::Connected: {
          if (l.dropped.exchange(false) || !l.client || !l.client->isConnected()) {
            disconnect();
            RadioLock lock(&r);
            l.stats_.drops++;
            l.stats_.connectedSinceMs = 0;
            copyStr(l.stats_.lastError, "connection lost", sizeof(l.stats_.lastError));
            l.nextTryMs = now + l.spec.backoffMinMs;
            setState(l, BleLinkState::Backoff);
            log_w("[ble] %s: connection lost", l.spec.owner);
            break;
          }
          if (now - l.lastRssiMs >= kRssiPeriodMs) {
            const int rssi = l.client->getRssi();
            RadioLock lock(&r);
            l.rssi_ = int8_t(rssi);
            l.lastRssiMs = now;
          }
          break;
        }

        case BleLinkState::Connecting:
          break;  // only inside the attempt above
      }
    }

    notifyStates();
    // Sleep until something happens (request, scan end, disconnect) or 200 ms.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
  }
}

#endif  // ESPRACK_HAVE_NIMBLE
