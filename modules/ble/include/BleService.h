#pragma once
#ifndef ESPRACK_BLE_SERVICE_H
#define ESPRACK_BLE_SERVICE_H

// BleService — the working half of BleModule and the IBleProvider
// implementation.
//
//   * BleState                 settings (/config/ble.json) + a runtime mirror
//                              for the UI, behind the StatefulService mutex
//   * the radio engine         NimBLE host, scan table and link slots, behind
//                              its own mutex, driven by the "ble" task
//   * feature "ble"            Status (live: radio, links), Scan (REST:
//                              results + ble.scan), Settings (admin)
//
// Threads:
//   * "ble" task (static stack): host start/stop, scans, the link state
//     machines and every blocking stack call (connect, discovery, subscribe);
//   * NimBLE host task: scan results and notifications (short, under _mx);
//   * loop task: mirrors the engine into BleState and pushes WS;
//   * any task: IBleProvider / IBleLink calls (lock _mx, wake the ble task).

#include <Arduino.h>
#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <ConfigDelegate.h>
#include <FormBuilder.h>
#include <StatefulService.h>
#include <WebFeatureDelegate.h>
#include <ble/IBleProvider.h>
#include <web/Web.h>

class WebManager;

#define BLE_MODULE_VERSION "0.1.0"
#define BLE_CONFIG_FILE "/config/ble.json"

// Link slots (also capped by the stack's CONFIG_BT_NIMBLE_MAX_CONNECTIONS).
#ifndef ESPRACK_BLE_MAX_LINKS
#define ESPRACK_BLE_MAX_LINKS 3
#endif
// Rows of the shared scan table.
#ifndef ESPRACK_BLE_SCAN_ROWS
#define ESPRACK_BLE_SCAN_ROWS 24
#endif
#ifndef ESPRACK_BLE_TASK_STACK
#define ESPRACK_BLE_TASK_STACK 6144
#endif

// Factory defaults; the consumer overrides them in factory_settings.ini.
#ifndef FACTORY_BLE_ENABLED
#define FACTORY_BLE_ENABLED true
#endif
#ifndef FACTORY_BLE_DEVICE_NAME
#define FACTORY_BLE_DEVICE_NAME ""  // "" = DeviceIdentity short id
#endif
#ifndef FACTORY_BLE_TX_POWER
#define FACTORY_BLE_TX_POWER 9  // dBm
#endif
#ifndef FACTORY_BLE_MTU
#define FACTORY_BLE_MTU 247
#endif

struct BleLinkRow {
  bool used = false;
  char owner[24] = "";
  char peer[ESPRack::kBleAddrLen] = "";
  ESPRack::BleLinkState state = ESPRack::BleLinkState::Disabled;
  int8_t rssi = 0;
  uint16_t mtu = 0;
  ESPRack::BleLinkStats stats;
};

class BleState {
 public:
  // ---- persisted ----
  bool enabled = FACTORY_BLE_ENABLED;
  String deviceName = FACTORY_BLE_DEVICE_NAME;
  int8_t txPower = FACTORY_BLE_TX_POWER;
  uint16_t mtu = FACTORY_BLE_MTU;
  uint16_t scanIntervalMs = 100;
  uint16_t scanWindowMs = 50;
  bool activeScan = true;
  uint8_t scanSeconds = 5;

  // ---- runtime mirror (never persisted) ----
  bool compiledIn = false;  // the stack is part of this build
  bool safeMode = false;
  bool hostUp = false;
  char ownAddr[ESPRack::kBleAddrLen] = "";
  char hostError[40] = "";
  uint8_t maxLinks = 0;
  uint8_t linkCount = 0;
  BleLinkRow links[ESPRACK_BLE_MAX_LINKS];
  bool scanning = false;
  uint32_t scanGen = 0;
  uint8_t scanCount = 0;
  ESPRack::BleScanRow scan[ESPRACK_BLE_SCAN_ROWS];

  static void readConfig(BleState& s, JsonObject& root);
  static StateUpdateResult update(JsonObject& root, BleState& s);
  static void buildForm(BleState& s, JsonObject& root);
  static void readState(BleState& s, JsonObject& root);
};

class BleService : public StatefulService<BleState>, public ESPRack::IBleProvider {
 public:
  explicit BleService(ConfigManager* cfgMgr);

  void registerManifest(WebManager* web);  // onInstall
  void begin();                            // onBegin
  void loop();                             // onLoop
  void end();                              // onShutdown

  // ---- IBleProvider (any task) ----
  bool ready() const override;
  void ownAddress(char* out, size_t len) const override;
  bool requestScan(uint32_t durationMs = 0) override;
  bool scanning() const override;
  uint32_t scanGeneration() const override;
  size_t scanResults(ESPRack::BleScanRow* out, size_t max) const override;
  ESPRack::IBleLink* registerLink(const ESPRack::BleLinkSpec& spec, const char** why = nullptr) override;
  uint8_t maxLinks() const override;
  uint8_t linkCount() const override;

  class Link;    // IBleLink implementation (BleService.cpp)
  struct Radio;  // stack-specific engine state (BleService.cpp)

 private:
  static void taskEntry(void* self);
  void taskLoop();
  void notifyStates();  // onState callbacks, ble task, no locks held
  void wake();
  void mirror();  // engine -> BleState, loop task

  ConfigDelegate<BleState> _cfg;
  WebFeatureEntry<BleState>* _feature{nullptr};
  Radio* _radio{nullptr};
  std::atomic<bool> _applyPending{false};
  uint32_t _lastMirrorMs{0};
  uint32_t _mirroredScanGen{0xFFFFFFFF};
};

#endif  // ESPRACK_BLE_SERVICE_H
