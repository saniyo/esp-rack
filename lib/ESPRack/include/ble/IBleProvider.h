#pragma once
#ifndef ESPRACK_BLE_IBLE_PROVIDER_H
#define ESPRACK_BLE_IBLE_PROVIDER_H

// Bluetooth LE, late-bound like the other providers (App::ble()). BLE is the
// chip's own radio, so it is an internal framework module (modules/ble,
// BleModule), not an external HW instance. It is the ONLY owner of the BLE
// host stack: nobody else initialises or deinitialises it.
//
// What a consumer gets:
//   * scanning on demand, results in a bounded table (scanResults());
//   * central links to GATT peripherals (registerLink()): the provider finds
//     the peer (fixed address or advertised name prefix / service UUID),
//     connects, subscribes to the notify characteristic, reconnects with
//     backoff and counts traffic. The consumer only writes and handles
//     notifications.
//
//   ESPRack::IBleProvider* ble = app->ble();   // null: BleModule not installed
//   ESPRack::BleLinkSpec spec;
//   spec.owner = "sensor";
//   spec.setNamePrefixes("SN-,TH-");            // or spec.setAddress("aa:bb:..", 0)
//   spec.setService("ffe0");
//   spec.setNotifyChar("ffe1");                 // write char defaults to it
//   spec.onNotify = [this](const uint8_t* d, size_t n) { feed(d, n); };
//   link_ = ble->registerLink(spec, &why);      // null + why when refused
//   ...
//   link_->write(frame, len);
//
// Threads: registerLink(), scan requests and every IBleLink method may be
// called from any task. onNotify runs on the BLE host task and onState on
// the module's BLE task: copy the data into your own locked state and
// return, never block there.
//
// No NimBLE (or other stack) types appear here, so a consumer depends on the
// core only; the module brings the stack.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <functional>

namespace ESPRack {

enum class BleLinkState : uint8_t {
  Disabled,     // switched off (link or radio), or the host is not running
  Discovering,  // scanning for a peer that matches the name prefix / service
  Connecting,   // connecting, discovering services, subscribing
  Connected,    // subscribed; write() works
  Backoff       // waiting before the next attempt
};

inline const char* bleLinkStateName(BleLinkState s) {
  switch (s) {
    case BleLinkState::Disabled:    return "Disabled";
    case BleLinkState::Discovering: return "Discovering";
    case BleLinkState::Connecting:  return "Connecting";
    case BleLinkState::Connected:   return "Connected";
    case BleLinkState::Backoff:     return "Backoff";
  }
  return "?";
}

// "aa:bb:cc:dd:ee:ff" + NUL
constexpr size_t kBleAddrLen = 18;
// A 128-bit UUID string "0000ffe0-0000-1000-8000-00805f9b34fb" + NUL
constexpr size_t kBleUuidLen = 37;

// Everything a link needs. Strings are copied by registerLink(), so
// temporaries are fine.
struct BleLinkSpec {
  // Who uses the link; shown in the BLE status table. A string literal (kept
  // as a pointer).
  const char* owner = "?";

  // Target. A non-empty address connects to that peer; an empty one makes the
  // provider discover a peer whose advertised name starts with one of
  // namePrefixes (comma-separated, e.g. "SN-,TH-") or that advertises
  // discoverService. addressType: 0 public, 1 random.
  char address[kBleAddrLen] = "";
  uint8_t addressType = 0;
  char namePrefixes[32] = "";
  char discoverService[kBleUuidLen] = "";

  // GATT: the service, the characteristic to subscribe to (notify or
  // indicate; "" = none) and the one to write ("" = the notify one).
  // 16-bit UUIDs as "ffe0", 128-bit in the usual dashed form.
  char service[kBleUuidLen] = "";
  char notifyChar[kBleUuidLen] = "";
  char writeChar[kBleUuidLen] = "";
  bool writeWithResponse = false;

  uint32_t connectTimeoutMs = 5000;
  uint32_t discoverScanMs = 5000;  // one discovery scan
  uint32_t backoffMinMs = 2000;    // after a failure; doubles up to backoffMaxMs
  uint32_t backoffMaxMs = 60000;
  bool enabled = true;

  // Notification / indication payload. BLE host task: copy and return.
  std::function<void(const uint8_t* data, size_t len)> onNotify;
  // State changes (Connected, Backoff after a drop, ...). BLE task.
  std::function<void(BleLinkState state)> onState;

  void setAddress(const char* addr, uint8_t type = 0) { copy(address, addr, sizeof(address)); addressType = type; }
  void setNamePrefixes(const char* csv) { copy(namePrefixes, csv, sizeof(namePrefixes)); }
  void setDiscoverService(const char* uuid) { copy(discoverService, uuid, sizeof(discoverService)); }
  void setService(const char* uuid) { copy(service, uuid, sizeof(service)); }
  void setNotifyChar(const char* uuid) { copy(notifyChar, uuid, sizeof(notifyChar)); }
  void setWriteChar(const char* uuid) { copy(writeChar, uuid, sizeof(writeChar)); }

 private:
  static void copy(char* dst, const char* src, size_t n) {
    if (!n) return;
    size_t i = 0;
    if (src) for (; i + 1 < n && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
  }
};

struct BleLinkStats {
  uint32_t connects = 0;     // successful connections (subscribed)
  uint32_t failures = 0;     // failed attempts (connect, GATT, subscribe)
  uint32_t drops = 0;        // connections lost after Connected
  uint32_t rxFrames = 0;     // notifications
  uint32_t rxBytes = 0;
  uint32_t txFrames = 0;     // successful writes
  uint32_t txBytes = 0;
  uint32_t txErrors = 0;
  uint32_t connectedSinceMs = 0;  // millis() of the current connection, 0 = none
  char lastError[40] = "";
};

// A registered link. Owned by the provider; valid until release().
class IBleLink {
 public:
  // Write to the write characteristic. false when not Connected or the stack
  // refused (counted in txErrors). With writeWithResponse it waits for the
  // peer's acknowledgement, so keep it off time-critical tasks.
  virtual bool write(const uint8_t* data, size_t len) = 0;

  virtual BleLinkState state() const = 0;
  bool connected() const { return state() == BleLinkState::Connected; }
  // The peer's address: the configured one, or the one discovery found ("" before).
  virtual void peerAddress(char* out, size_t len) const = 0;
  virtual int8_t rssi() const = 0;      // dBm, last reading while Connected; 0 = unknown
  virtual uint16_t mtu() const = 0;     // negotiated ATT MTU; 0 when not connected
  virtual BleLinkStats stats() const = 0;

  // New target: "" = discover by the spec's name prefixes / service. Drops the
  // current connection when the target changed.
  virtual void setTarget(const char* address, uint8_t addressType = 0) = 0;
  virtual void setEnabled(bool enabled) = 0;
  // Drop the connection and try again now (backoff reset).
  virtual void reconnect() = 0;
  // Disconnect and free the slot. The pointer is invalid afterwards.
  virtual void release() = 0;

 protected:
  ~IBleLink() = default;
};

struct BleScanRow {
  char name[24];
  char address[kBleAddrLen];
  uint8_t addressType;
  int8_t rssi;
  bool connectable;
  uint16_t mfgId;          // first manufacturer data company id, 0xFFFF = none
  char service[kBleUuidLen];  // first advertised service UUID, "" = none
  uint32_t lastSeenMs;     // millis()
};

class IBleProvider {
 public:
  virtual ~IBleProvider() = default;

  // The host stack is running (enabled, not in BootGuard safe mode).
  virtual bool ready() const = 0;
  // Own BLE address, "" while the host is down.
  virtual void ownAddress(char* out, size_t len) const = 0;

  // Start a scan (0 = the configured duration). false while the radio is
  // down. A scan already running is extended, not restarted. Results land
  // in the shared table; scanGeneration() bumps when a scan ends.
  virtual bool requestScan(uint32_t durationMs = 0) = 0;
  virtual bool scanning() const = 0;
  virtual uint32_t scanGeneration() const = 0;
  // Copies up to max rows (strongest first); returns how many.
  virtual size_t scanResults(BleScanRow* out, size_t max) const = 0;

  // A central link. nullptr when refused (no free slot, radio disabled at
  // build time); *why then says why.
  virtual IBleLink* registerLink(const BleLinkSpec& spec, const char** why = nullptr) = 0;
  virtual uint8_t maxLinks() const = 0;
  virtual uint8_t linkCount() const = 0;
};

}  // namespace ESPRack

#endif  // ESPRACK_BLE_IBLE_PROVIDER_H
