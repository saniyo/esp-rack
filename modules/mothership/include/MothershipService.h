#pragma once
#ifndef MothershipService_h
#define MothershipService_h

// MothershipService — periodic HTTPS check-in to the fleet management
// server, command dispatcher for the response actions.
//
// Endpoint model — two-slot profiles ("Home" / "Work" etc.) so the
// operator can park two saved server configs and flip with a
// dropdown when the device moves networks. Server contract is fixed
// per instance (/api/v1/enroll, /api/v1/checkin, /api/v1/recover)
// — one Base URL per slot feeds all three. CertManagerService
// reads enroll + recover URLs through IMothershipProfileProvider
// at request time, so a profile switch in the UI applies to PKI
// flows transparently.

#include <StatefulService.h>
#include <ConfigManager.h>
#include <ConfigDelegate.h>
#include <FormBuilder.h>
#include <WebFeatureDelegate.h>

#include "IMothershipProvider.h"
#include "IMothershipProfileProvider.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <vector>

#define MOTHERSHIP_FILE      "/config/mothership.json"
#define MOTHERSHIP_FORM_PATH "/rest/mothership"
#define MOTHERSHIP_WS_PATH   "/ws/mothership"

// Build-time defaults — operator overrides via UI at runtime. Two
// named profiles get seeded on first boot ("Home" pointing at
// FACTORY_MOTHERSHIP_BASE_URL, "Work" pointing at the same as a
// blank-ready slot the operator can repoint later). The operator
// edits names and URLs through the UI; we never re-seed.
#ifndef FACTORY_MOTHERSHIP_BASE_URL
#define FACTORY_MOTHERSHIP_BASE_URL "https://mothership.local:8443"
#endif
#ifndef FACTORY_MOTHERSHIP_INTERVAL_MIN
#define FACTORY_MOTHERSHIP_INTERVAL_MIN 5
#endif

// Server-contract paths — same on every mothership instance.
// Profile.base_url is the host:port prefix; these get appended.
#define MOTHERSHIP_PATH_ENROLL   "/api/v1/enroll"
#define MOTHERSHIP_PATH_CHECKIN  "/api/v1/checkin"
#define MOTHERSHIP_PATH_RECOVER  "/api/v1/recover"

// Two profile slots — operator typically toggles between "home" and
// "work" / "office" / "staging". Fixed-size keeps the form schema
// flat (no dynamic array widget needed) and the response small
// enough for the buffered REST endpoint.
#ifndef MOTHERSHIP_PROFILE_SLOTS
#define MOTHERSHIP_PROFILE_SLOTS 2
#endif

class WebManager;
class ITLSProvider;
class ICertProvider;
class IWireguardProvider;

// A named mothership endpoint. base_url is "https://host:port" with
// no trailing slash and no /api/v1/... suffix — the suffix is fixed
// per server contract and derived at request time.
struct MothershipProfile {
  String name;
  String base_url;
};

struct MothershipSettings {
  // ── Persisted config ──
  bool     enabled{false};
  uint16_t interval_min{FACTORY_MOTHERSHIP_INTERVAL_MIN};

  // Named endpoint profiles (e.g. "Home" + "Work"). Editable in UI
  // via fixed slots; unused slots have empty name + base_url.
  std::vector<MothershipProfile> profiles;
  String   active_name;   // matches one profiles[i].name; empty = no
                          // active profile (mothership disabled in
                          // effect — getters below return "")

  // ── Runtime state (not persisted) ──
  IMothershipProvider::State runtime_state{IMothershipProvider::State::Disabled};
  String   status_label{"Disabled"};
  uint32_t last_checkin_at_s{0};
  uint32_t next_checkin_at_s{0};
  uint32_t success_count{0};
  uint32_t fail_count{0};

  // ── Derived URL helpers ──
  // Look up base_url of the profile whose name matches active_name.
  // Empty string when no match (no profile selected or stale name).
  String activeBaseUrl() const {
    if (active_name.length() == 0) return String();
    for (const auto& p : profiles) {
      if (p.name == active_name) return p.base_url;
    }
    return String();
  }
  String checkinUrl() const {
    String b = activeBaseUrl();
    return b.length() ? b + MOTHERSHIP_PATH_CHECKIN : String();
  }
  String enrollUrl() const {
    String b = activeBaseUrl();
    return b.length() ? b + MOTHERSHIP_PATH_ENROLL : String();
  }
  String recoverUrl() const {
    String b = activeBaseUrl();
    return b.length() ? b + MOTHERSHIP_PATH_RECOVER : String();
  }

  static void readConfig(MothershipSettings& s, JsonObject& root);
  static StateUpdateResult update(JsonObject& root, MothershipSettings& s);
  static void buildForm(MothershipSettings& s, JsonObject& root);
  static void staRead(MothershipSettings& s, JsonObject& root);
  static StateUpdateResult staUpd(JsonObject& root, MothershipSettings& s);
};

class MothershipService : public StatefulService<MothershipSettings>,
                          public IMothershipProvider,
                          public IMothershipProfileProvider {
 public:
  MothershipService(ConfigManager* cfgMgr,
                    ITLSProvider* tls,
                    ICertProvider* cert,
                    IWireguardProvider* wg = nullptr);

  void registerManifest(WebManager* web);
  void begin();
  void loop();

  // IMothershipProvider
  State state() const override { return _state.runtime_state; }
  int32_t lastCheckInAgoSec() const override;
  int32_t nextCheckInInSec() const override;
  uint32_t successCount() const override { return _state.success_count; }
  uint32_t failCount()    const override { return _state.fail_count; }

  // IMothershipProfileProvider — delegate to settings.
  // Cert-manager picks these up at request time, so a profile
  // switch in the UI takes effect on next enroll/recover poll.
  String activeName()    const override { return _state.active_name; }
  String activeBaseUrl() const override { return _state.activeBaseUrl(); }
  String enrollUrl()     const override { return _state.enrollUrl(); }
  String checkinUrl()    const override { return _state.checkinUrl(); }
  String recoverUrl()    const override { return _state.recoverUrl(); }

 private:
  ConfigDelegate<MothershipSettings>      _cfg;
  WebFeatureEntry<MothershipSettings>*    _feature{nullptr};
  ITLSProvider*                            _tls{nullptr};
  ICertProvider*                           _cert{nullptr};
  // Optional — populated by MothershipModule from app->wireguard()
  // when WireGuardModule is installed in the consumer. Null when
  // the consumer doesn't want tunneling.
  IWireguardProvider*                      _wg{nullptr};

  // Refresh runtime_state + status_label from persisted fields +
  // CertProvider readiness. Runs in begin(), after every update,
  // and at the end of every check-in attempt.
  void refreshRuntimeState();

  // ── Check-in scheduling ──
  TaskHandle_t   _task{nullptr};
  static void    checkinTaskTramp(void* arg);
  void           runCheckinLoop();

  // Single check-in iteration: build request → POST → parse →
  // dispatch actions → return success bool. outBurst is set true
  // when at least one action was dispatched — signals the adaptive
  // cadence to schedule next tick at 10 s instead of the base
  // interval (likely more queued).
  bool           performOneCheckin(bool& outBurst);

  // Dispatch the actions[] array from the check-in response. Returns
  // true if any action was dispatched (used by adaptive cadence to
  // trigger 10 s burst follow-up).
  bool           dispatchActions(JsonArrayConst actions);

  // Per-action handlers. Each takes the action's `params` JSON
  // object and returns a brief result string (logged in serial,
  // surfaced via WS in Phase 2.4 command log).
  String         actionUpdate(JsonObjectConst params);
  String         actionRenewCert(JsonObjectConst params);
  String         actionOpenTunnel(JsonObjectConst params);
  String         actionCloseTunnel(JsonObjectConst params);
  String         actionSetConfig(JsonObjectConst params);
  String         actionReboot(JsonObjectConst params);
  String         actionLog(JsonObjectConst params);
};

#endif  // MothershipService_h
