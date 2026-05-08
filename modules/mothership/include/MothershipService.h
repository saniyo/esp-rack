#pragma once
#ifndef MothershipService_h
#define MothershipService_h

// MothershipService — periodic HTTPS check-in to the fleet management
// server, command dispatcher for the response actions. Phase 2 of
// the mothership roadmap (see docs/plans/mothership-roadmap.md).
//
// Phase 2.0 SKELETON: state struct, ConfigDelegate persistence, UI
// stub, IMothershipProvider impl with hard-coded "Disabled". All
// real work arrives in subsequent commits:
//
//   Phase 2.1 — Settings UI (URL, interval, enabled) + persistence
//   Phase 2.2 — Adaptive polling FreeRTOS task + HTTPS POST through
//                TLSContextService (mTLS via CertManager-loaded cert)
//   Phase 2.3 — Command dispatcher (update / openTunnel / renewCert /
//                setConfig / reboot / log)
//   Phase 2.4 — UI command log + live readouts (last/next check-in)
//   Phase 2.5 — Mock server /api/v1/checkin for end-to-end testing

#include <StatefulService.h>
#include <ConfigManager.h>
#include <ConfigDelegate.h>
#include <FormBuilder.h>
#include <WebFeatureDelegate.h>

#include "IMothershipProvider.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define MOTHERSHIP_FILE      "/config/mothership.json"
#define MOTHERSHIP_FORM_PATH "/rest/mothership"
#define MOTHERSHIP_WS_PATH   "/ws/mothership"

// Build-time defaults — operator overrides via UI at runtime.
#ifndef FACTORY_MOTHERSHIP_CHECKIN_URL
#define FACTORY_MOTHERSHIP_CHECKIN_URL "https://mothership.local:8443/api/v1/checkin"
#endif
#ifndef FACTORY_MOTHERSHIP_INTERVAL_MIN
#define FACTORY_MOTHERSHIP_INTERVAL_MIN 5
#endif

class WebManager;
class ITLSProvider;
class ICertProvider;

struct MothershipSettings {
  // ── Persisted config ──
  bool     enabled{false};                     // master toggle
  String   checkin_url{FACTORY_MOTHERSHIP_CHECKIN_URL};
  uint16_t interval_min{FACTORY_MOTHERSHIP_INTERVAL_MIN};

  // ── Runtime state (not persisted) ──
  IMothershipProvider::State runtime_state{IMothershipProvider::State::Disabled};
  String   status_label{"Disabled"};
  uint32_t last_checkin_at_s{0};   // seconds since boot of last successful checkin
  uint32_t next_checkin_at_s{0};   // scheduled next checkin (seconds since boot)
  uint32_t success_count{0};
  uint32_t fail_count{0};

  static void readConfig(MothershipSettings& s, JsonObject& root);
  static StateUpdateResult update(JsonObject& root, MothershipSettings& s);
  static void buildForm(MothershipSettings& s, JsonObject& root);
  static void staRead(MothershipSettings& s, JsonObject& root);
  static StateUpdateResult staUpd(JsonObject& root, MothershipSettings& s);
};

class MothershipService : public StatefulService<MothershipSettings>,
                          public IMothershipProvider {
 public:
  MothershipService(ConfigManager* cfgMgr,
                    ITLSProvider* tls,
                    ICertProvider* cert);

  void registerManifest(WebManager* web);
  void begin();
  void loop();

  // IMothershipProvider
  State state() const override { return _state.runtime_state; }
  int32_t lastCheckInAgoSec() const override;
  int32_t nextCheckInInSec() const override;
  uint32_t successCount() const override { return _state.success_count; }
  uint32_t failCount()    const override { return _state.fail_count; }

 private:
  ConfigDelegate<MothershipSettings>      _cfg;
  WebFeatureEntry<MothershipSettings>*    _feature{nullptr};
  ITLSProvider*                            _tls{nullptr};
  ICertProvider*                           _cert{nullptr};

  // Refresh runtime_state + status_label from persisted fields +
  // CertProvider readiness. Runs in begin(), after every update,
  // and at the end of every check-in attempt.
  void refreshRuntimeState();
};

#endif  // MothershipService_h
