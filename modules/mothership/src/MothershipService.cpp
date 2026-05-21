#include <MothershipService.h>
#include <ITLSProvider.h>
#include <ICertProvider.h>
#include <IWireguardProvider.h>
#include <DeviceIdentity.h>
#include <WebManager.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_system.h>

// ===== Persistence =====
//
// readConfig / update are flat sequences — no loops, no conditionals,
// no snprintf'd keys. See feedback_static_forms memory for the
// catastrophic-debugging story behind this rule. Every JSON key is a
// string literal so ArduinoJson's zero-copy linked-string semantics
// have a stable backing.

void MothershipSettings::readConfig(MothershipSettings& s, JsonObject& root) {
  root["enabled"]        = s.enabled;
  root["interval_s"]     = s.interval_s;
  root["active_idx"]     = s.active_idx;
  root["profile_0_name"] = s.profile_a.name;
  root["profile_0_url"]  = s.profile_a.base_url;
  root["profile_1_name"] = s.profile_b.name;
  root["profile_1_url"]  = s.profile_b.base_url;
}

StateUpdateResult MothershipSettings::update(JsonObject& root,
                                              MothershipSettings& s) {
  bool ch = false;
  ch |= FormBuilder::updateValue(root, "enabled",        s.enabled);
  ch |= FormBuilder::updateValue(root, "interval_s",     s.interval_s);

  // Backward-compat migration: existing /config/mothership.json from
  // before the seconds-rename still carries `interval_min` (minutes)
  // and no `interval_s`. On first boot after upgrade we convert
  // minutes → seconds once and mark CHANGED so the next save rewrites
  // the field under its new name. After that one save the legacy key
  // never appears again. This is the ONLY conditional in update() —
  // it's a one-time compatibility shim, not a render-time branch.
  if (!root.containsKey("interval_s") && root.containsKey("interval_min")) {
    uint32_t legacy_min = root["interval_min"].as<uint32_t>();
    uint32_t as_seconds = legacy_min * 60u;
    if (as_seconds > 65535u) as_seconds = 65535u;
    if (s.interval_s != (uint16_t)as_seconds) {
      s.interval_s = (uint16_t)as_seconds;
      ch = true;
    }
  }

  ch |= FormBuilder::updateValue(root, "active_idx",     s.active_idx);
  ch |= FormBuilder::updateValue(root, "profile_0_name", s.profile_a.name);
  ch |= FormBuilder::updateValue(root, "profile_0_url",  s.profile_a.base_url);
  ch |= FormBuilder::updateValue(root, "profile_1_name", s.profile_b.name);
  ch |= FormBuilder::updateValue(root, "profile_1_url",  s.profile_b.base_url);
  return ch ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
}

// ===== Form schema =====
//
// Fully static — zero loops, zero conditionals, zero derived values
// computed during the GET. Every addX() call hands FormBuilder a
// direct state field; the frontend takes care of presentation. The
// only "logic" lives in the helper getters of MothershipSettings
// (activeBaseUrl, etc.) which run only when the check-in task
// actually needs a URL, never on form render.
//
// All JSON keys are string literals — see feedback_static_forms
// memory for why snprintf'd keys are catastrophic.
void MothershipSettings::buildForm(MothershipSettings& s, JsonObject& root) {
  // ── STATUS ───────────────────────────────────────────────────────
  JsonArray st = FormBuilder::createForm(root, "status",
                                          "Mothership client status");
  FormBuilder::addTextField(st, "status", AF::R, s.status_label.c_str(),
                            label("State"), icon("Cloud"),
                            colorMap("LastOk:success,CheckingIn:info,"
                                     "Idle:info,LastFail:error,"
                                     "NeedsCert:warning,Disabled:default,"
                                     "default:info"));
  FormBuilder::addNumberField(st, "success_count", AF::R,
                              (double)s.success_count, format("0"),
                              label("Successful check-ins"),
                              icon("CheckCircleOutline"));
  FormBuilder::addNumberField(st, "fail_count", AF::R,
                              (double)s.fail_count, format("0"),
                              label("Failed check-ins"),
                              icon("ErrorOutline"));

  // ── SETTINGS ─────────────────────────────────────────────────────
  JsonArray set = FormBuilder::createForm(root, "settings",
                                           "Mothership client config");
  FormBuilder::addSwitchField(set, "enabled", AF::RW, s.enabled,
                              label("Enabled"), icon("PowerSettingsNew"));
  FormBuilder::addNumberField(set, "interval_s", AF::RW,
                              (double)s.interval_s,
                              minVal(5), maxVal(3600), format("0"),
                              label("Check-in interval"),
                              icon("Timer"), unit("s"));

  // ── PROFILES ─────────────────────────────────────────────────────
  JsonArray pf = FormBuilder::createForm(root, "profiles",
                                          "Mothership profiles");
  // Dropdown — literal labels, integer values. Frontend can fancy up
  // the display (slot + URL) if needed; the form-build path stays
  // loop-free.
  FormBuilder::addDropdownField(pf, "active_idx", AF::RW,
                                (int)s.active_idx,
                                opt("Profile 1", 0),
                                opt("Profile 2", 1),
                                label("Active profile"),
                                icon("CheckCircle"));
  // Per-slot editor — keys MUST be string literals (not stack
  // char-arrays), because ArduinoJson stores `field[const char*]` as
  // a zero-copy linked string. A `char buf[32]` from a `for` loop
  // body goes out of scope before AsyncJsonResponse serializes, and
  // all four keys end up pointing at the same garbage stack slot —
  // which is exactly why typing into Profile 1's name field made
  // Profile 2's name field appear to change too. Unrolled, slot
  // count = 2 baked into the form shape.
  FormBuilder::addTextField(pf, "profile_0_name", AF::RW,
                            s.profile_a.name.c_str(),
                            label("Profile 1 name"), icon("Label"));
  FormBuilder::addTextField(pf, "profile_0_url", AF::RW,
                            s.profile_a.base_url.c_str(),
                            label("Profile 1 base URL"),
                            placeholder("https://host:8443"),
                            icon("Cloud"));
  FormBuilder::addTextField(pf, "profile_1_name", AF::RW,
                            s.profile_b.name.c_str(),
                            label("Profile 2 name"), icon("Label"));
  FormBuilder::addTextField(pf, "profile_1_url", AF::RW,
                            s.profile_b.base_url.c_str(),
                            label("Profile 2 base URL"),
                            placeholder("https://host:8443"),
                            icon("Cloud"));
}

// ===== WS push =====
// Removed — Status tab is live=false in registerManifest. The
// React UI re-fetches via REST when the operator switches back to
// the tab; no need for tick-driven WS pushes here, and dropping
// them eliminates the WS-vs-REST race that was stalling the
// spinner on tab switch.

// ===== Service =====

MothershipService::MothershipService(ConfigManager* cfgMgr,
                                     ITLSProvider* tls,
                                     ICertProvider* cert,
                                     IWireguardProvider* wg)
    : StatefulService<MothershipSettings>(),
      _cfg(cfgMgr,
           "mothership",
           MOTHERSHIP_FILE,
           4096,
           this,
           MothershipSettings::readConfig,
           MothershipSettings::update,
           false /*autoSave*/,
           nullptr /*validator*/,
           MothershipSettings::buildForm /*formReader*/),
      _tls(tls),
      _cert(cert),
      _wg(wg) {
  addUpdateHandler([this](const String& origin) {
    // Refresh state-machine ONLY for non-tick origins. The tick path
    // (mship.tick-start / mship.tick-end) sets runtime_state
    // authoritatively in its lambda — Enrolling/CheckingIn/LastOk/
    // LastFail. Calling refreshRuntimeState here would derive state
    // from counter comparisons (fail_count > success_count etc.) and
    // clobber the freshly-set value: e.g. tick-end sets LastOk after
    // a successful check-in, refresh sees fail_count(6) > success(2)
    // from history and flips back to LastFail. Only form-save / WS
    // updates / boot need the counter-driven derivation.
    if (!origin.startsWith("mship.tick")) {
      refreshRuntimeState();
    }
    _cfg.saveIfChanged(origin);
    if (_feature) _feature->broadcastWs(origin);
  }, false);
}

void MothershipService::registerManifest(WebManager* web) {
  if (!web) return;

  WebFeatureSpec spec;
  spec.id         = "mothership";
  spec.title      = "Mothership";
  spec.component  = "DynamicSettings";
  spec.menu.label = "Mothership";
  spec.menu.icon  = "Cloud";
  spec.menu.order = 370;
  spec.menu.auth  = WebAuthLevel::Admin;
  spec.auth       = WebAuthLevel::Admin;
  spec.restRead   = MOTHERSHIP_FORM_PATH;
  spec.restUpdate = MOTHERSHIP_FORM_PATH;

  WebTabSpec statusTab;
  statusTab.key      = "status";
  statusTab.title    = "Status";
  statusTab.restPath = MOTHERSHIP_FORM_PATH;
  statusTab.postable = false;
  // live=false intentionally — Status fields are flat counters +
  // label; the operator can pull-to-refresh by re-opening the tab.
  // The WS-tick path used to race REST GET on tab-switch and stall
  // the spinner; dropping the live=true sidesteps that.
  statusTab.live     = false;
  spec.tabs.push_back(statusTab);

  WebTabSpec settingsTab;
  settingsTab.key      = "settings";
  settingsTab.title    = "Settings";
  settingsTab.restPath = MOTHERSHIP_FORM_PATH;
  settingsTab.postable = true;
  settingsTab.auth     = WebAuthLevel::Admin;
  settingsTab.order    = 20;
  spec.tabs.push_back(settingsTab);

  WebTabSpec profilesTab;
  profilesTab.key      = "profiles";
  profilesTab.title    = "Profiles";
  profilesTab.restPath = MOTHERSHIP_FORM_PATH;
  profilesTab.postable = true;
  profilesTab.auth     = WebAuthLevel::Admin;
  profilesTab.order    = 30;
  spec.tabs.push_back(profilesTab);

  // Two-arg overload uses the same reader/updater for REST only —
  // no separate WS variant because there's no WS subscription
  // (status tab is live=false). 16 KB REST buffer covers all three
  // sections comfortably.
  _feature = web->registerFeature<MothershipSettings>(
      std::move(spec), this,
      MothershipSettings::buildForm,  MothershipSettings::update,
      16384);
}

void MothershipService::begin() {
  (void)_cfg.ensureLoaded();
  // No seed step needed — in-struct initialisers on profile_a /
  // profile_b / active_idx already give the empty-config first boot
  // a sane defaults state. ensureLoaded overlays any persisted
  // values; missing keys preserve the in-struct defaults.

  refreshRuntimeState();

  // Spawn the check-in task once at boot. Inside the loop the task
  // checks `_state.enabled` + cert availability each iteration —
  // toggling enabled doesn't restart the task, just changes whether
  // the next iteration POSTs or sleeps. 8 KB stack matches the cert-
  // manager enrollment task: mbedtls TLS handshake + JSON response
  // parsing under one stack.
  if (_task == nullptr) {
    BaseType_t rc = xTaskCreatePinnedToCore(
        &MothershipService::checkinTaskTramp,
        "mothership",
        8192,
        this,
        1,
        &_task,
        tskNO_AFFINITY);
    if (rc != pdPASS) {
      Serial.printf("[mship.begin] xTaskCreate failed: %d\n", (int)rc);
      _task = nullptr;
    }
  }

  if (_feature) _feature->broadcastWs("boot");
}

void MothershipService::loop() {
  // Phase 2.4 will tick the live last/next countdowns via WS push
  // here. Actual check-in lives in the FreeRTOS task spawned in
  // begin(); loop() stays cooperative-cheap.
}

// ===== Phase 2.2 — Check-in task =====
//
// Single long-running task. Each iteration:
//   1. Sleep until either next_checkin_at_s (fixed wall-clock) or a
//      wakeup notification (burst after pending actions, Phase 2.3).
//   2. Re-check enabled + cert readiness; if either off, just sleep
//      another interval. The task NEVER exits — toggling enabled
//      just suspends iterations.
//   3. performOneCheckin(); update counters; schedule next iteration
//      (5 min default, or 10s if dispatchActions returned true).

void MothershipService::checkinTaskTramp(void* arg) {
  auto* svc = static_cast<MothershipService*>(arg);
  if (svc) svc->runCheckinLoop();
  vTaskDelete(nullptr);
}

void MothershipService::runCheckinLoop() {
  Serial.println("[mship] check-in task started");
  uint32_t loopIter = 0;

  while (true) {
    loopIter++;
    // Re-evaluate gates each iteration.
    bool en   = _state.enabled;
    bool hasC = _cert && _cert->hasValidCert();
    bool wlan = WiFi.isConnected();
    bool wantTick = en && hasC && wlan;

    // Diagnostic every 6 iterations (=30s) so we can see the task
    // is alive and what's gating it. Also fires immediately at iter 1.
    if (loopIter == 1 || (loopIter % 6) == 0) {
      Serial.printf("[mship.loop iter=%u] enabled=%d hasCert=%d wifi=%d "
                    "wantTick=%d nextAt=%u nowAt=%u active='%s'\n",
                    (unsigned)loopIter, (int)en, (int)hasC, (int)wlan,
                    (int)wantTick,
                    (unsigned)_state.next_checkin_at_s,
                    (unsigned)(millis() / 1000),
                    (_state.active_idx == 0
                      ? _state.profile_a.name.c_str()
                      : _state.profile_b.name.c_str()));
    }

    if (!wantTick) {
      vTaskDelay(pdMS_TO_TICKS(5000));   // recheck every 5s when paused
      continue;
    }

    // First-tick optimisation: if next_checkin_at_s is 0 (never
    // scheduled) OR in the past, fire immediately. Otherwise wait
    // until it elapses.
    uint32_t cur_s = (uint32_t)(millis() / 1000);
    if (_state.next_checkin_at_s != 0 && cur_s < _state.next_checkin_at_s) {
      // Sleep until next_checkin_at_s; wake every 5s to pick up an
      // operator pause / interval change without waiting full duration.
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    // Schedule next iteration BEFORE sending the request — so the
    // status countdown immediately ticks toward the next moment, not
    // toward "now + handshake time".
    uint32_t now_s = (uint32_t)(millis() / 1000);
    uint32_t base_interval_s = (uint32_t)_state.interval_s;
    // 5 s sanity floor — matches the form's minVal. If a corrupt
    // save yields 0/1/2 the device still keeps a workable polling
    // rate instead of spinning at full speed.
    if (base_interval_s < 5) base_interval_s = 5;
    update([base_interval_s, now_s](MothershipSettings& s) {
      s.next_checkin_at_s = now_s + base_interval_s;
      s.runtime_state     = IMothershipProvider::State::CheckingIn;
      s.status_label      = "CheckingIn";
      return StateUpdateResult::CHANGED;
    }, "mship.tick-start");

    bool burst = false;
    bool ok = performOneCheckin(burst);

    update([ok, burst, now_s, base_interval_s](MothershipSettings& s) {
      if (ok) {
        s.success_count++;
        s.last_checkin_at_s = now_s;
        s.runtime_state = IMothershipProvider::State::LastOk;
        s.status_label  = "LastOk";
      } else {
        s.fail_count++;
        s.runtime_state = IMothershipProvider::State::LastFail;
        s.status_label  = "LastFail";
      }
      // Adaptive cadence: if actions arrived this round, drop to
      // 10-second burst follow-up — likely more queued.
      uint32_t interval = burst ? 10u : base_interval_s;
      s.next_checkin_at_s = now_s + interval;
      return StateUpdateResult::CHANGED;
    }, "mship.tick-end");

    // Note: refreshRuntimeState() is intentionally NOT called here —
    // the lambda above sets the post-tick state directly. Calling
    // refresh would re-derive from counters and could clobber the
    // freshly-recorded LastOk/LastFail with a stale Idle reading
    // (e.g. when fail_count == success_count midway through a fail
    // streak). The refresh path is reserved for cases where the
    // state-machine inputs (cert availability, enabled toggle) might
    // have changed independently of the tick — boot, form save.
    if (_feature) _feature->broadcastWs("mship.tick");

    // Top-of-loop check (next iteration) handles the wait until the
    // next scheduled check-in via vTaskDelay(5000) + cur_s vs
    // next_checkin_at_s comparison. No explicit inner-wait needed.
  }
}

bool MothershipService::performOneCheckin(bool& outBurst) {
  outBurst = false;
  // Snapshot URL up-front — derived from active profile at this
  // moment. If the operator switches profile mid-flight the current
  // tick finishes against the old URL and the next tick picks up
  // the new one.
  String checkin_url = _state.checkinUrl();
  Serial.printf("[mship.do] enter — tls=%p cert=%p url-len=%u\n",
                (void*)_tls, (void*)_cert,
                (unsigned)checkin_url.length());
  if (!_tls || !_cert) {
    Serial.println("[mship.do] EARLY: missing tls or cert provider");
    return false;
  }
  if (checkin_url.length() == 0) {
    Serial.println("[mship.do] EARLY: no active mothership profile");
    return false;
  }

  WiFiClientSecure client;
  Serial.println("[mship.do] step1: attachToClient");
  _tls->attachToClient(client);
  client.setHandshakeTimeout(15);
  client.setTimeout(15000);
  Serial.println("[mship.do] step2: timeouts set");

  HTTPClient http;
  Serial.printf("[mship.do] step3: http.begin(%s)\n",
                checkin_url.c_str());
  if (!http.begin(client, checkin_url)) {
    Serial.println("[mship.do] EARLY: http.begin failed");
    return false;
  }
  Serial.println("[mship.do] step4: http.begin OK");
  http.setTimeout(15000);
  http.addHeader("Content-Type", "application/json");

  // Body: {deviceId, fwVer, hwVer, hwRev, uptimeSec, freeHeap}
  // All identity fields come from the single source of truth in
  // DeviceIdentity so this payload stays in lockstep with the
  // X.509 cert subject + AutoUpdate URL + Identity tab readout.
  // mTLS handshake guarantees the device IS that subject CN; the
  // JSON-side deviceId is for human-readable log lines / DB row
  // lookups only.
  DynamicJsonDocument req(1024);
  req["deviceId"]  = _cert->subjectCN();
  req["fwVer"]     = DeviceIdentity::version();
  req["hwVer"]     = DeviceIdentity::chipModelFull();   // "ESP32-S3-N16R8V"
  if (DeviceIdentity::hwRevision().length() > 0) {
    req["hwRev"]   = DeviceIdentity::hwRevision();
  }
  req["uptimeSec"] = (uint32_t)(millis() / 1000);
  req["freeHeap"]  = ESP.getFreeHeap();
  String body;
  serializeJson(req, body);

  Serial.printf("[mship] POST %s, body=%u B\n",
                checkin_url.c_str(),
                (unsigned)body.length());

  int code = http.POST(body);
  Serial.printf("[mship] HTTP code=%d\n", code);

  if (code < 200 || code >= 300) {
    String err = http.getString();
    Serial.printf("[mship] server error: %s\n", err.c_str());
    http.end();
    return false;
  }

  String respBody = http.getString();
  http.end();
  Serial.printf("[mship] response %u B\n", (unsigned)respBody.length());

  // Response: {"actions": [...], "nextCheckInSec": N}
  // 16 KB doc — actions can carry firmware URLs / WireGuard configs.
  DynamicJsonDocument resp(16384);
  DeserializationError jerr = deserializeJson(resp, respBody);
  if (jerr) {
    Serial.printf("[mship] JSON parse: %s\n", jerr.c_str());
    return false;
  }

  // Phase 2.3 — dispatch actions; outBurst signals to caller that
  // at least one action was dispatched, triggering 10s burst
  // cadence on the next tick (likely more actions queued).
  if (resp["actions"].is<JsonArray>()) {
    outBurst = dispatchActions(resp["actions"].as<JsonArrayConst>());
  }

  return true;
}

// ===== Phase 2.3 — Action dispatcher =====

bool MothershipService::dispatchActions(JsonArrayConst actions) {
  bool any = false;
  for (JsonVariantConst v : actions) {
    if (!v.is<JsonObjectConst>()) continue;
    JsonObjectConst a = v.as<JsonObjectConst>();
    String type = a["type"].as<String>();
    JsonObjectConst params = a["params"].as<JsonObjectConst>();
    String result;

    if (type == "update")          result = actionUpdate(params);
    else if (type == "renewCert")   result = actionRenewCert(params);
    else if (type == "openTunnel")  result = actionOpenTunnel(params);
    else if (type == "closeTunnel") result = actionCloseTunnel(params);
    else if (type == "setConfig")   result = actionSetConfig(params);
    else if (type == "reboot")      result = actionReboot(params);
    else if (type == "log")         result = actionLog(params);
    else                             result = "unknown action type";

    Serial.printf("[mship.action] %s → %s\n", type.c_str(), result.c_str());
    any = true;
  }
  return any;
}

String MothershipService::actionUpdate(JsonObjectConst params) {
  // Phase 2.5 implements an INDEPENDENT mTLS-fetched OTA path here —
  // mothership-driven updates flow through TLSContextService and
  // bypass AutoUpdateService entirely. Rationale: AutoUpdate is the
  // back-door (plain HTTP polling against a hard-coded URL) that
  // MUST keep working even when mothership is unreachable / cert
  // expired / server is rolled back. Two parallel update paths,
  // each owns its own crypto + transport + signature checks.
  // For now log + acknowledge so end-to-end test can verify the
  // dispatcher path works.
  String url = params["url"].as<String>();
  if (url.length() == 0) return "missing url";
  Serial.printf("[mship.action.update] would download %s\n", url.c_str());
  return "deferred to phase 2.5";
}

String MothershipService::actionRenewCert(JsonObjectConst params) {
  // Phase 4 wires this into CertManagerService::rotate(). For now
  // ack so the dispatcher logs work end-to-end.
  (void)params;
  return "deferred to phase 4";
}

String MothershipService::actionOpenTunnel(JsonObjectConst params) {
  // Phase 3 — bring the device's WireGuard tunnel up using the
  // triplet the mothership supplies in the action params.
  //
  // Params shape:
  //   { "server_pubkey": "...base64...",
  //     "endpoint":      "1.2.3.4:51820",
  //     "assigned_ip":   "10.99.0.7" }
  //
  // Any/all fields can be empty — IWireguardProvider::up() falls
  // back to its cached values from the previous openTunnel cycle.
  // The tunnel goes up synchronously here; the actual handshake
  // completes asynchronously in the lib's internal task. Operator-
  // facing "tunnel is live" signal is via the action_result we
  // push to the ring (status 200) plus a follow-up status read
  // on the device's check-in payload (next pass after up()).
  if (!_wg) {
    return "no wireguard provider — install WireGuardModule";
  }
  String srv = params["server_pubkey"].as<String>();
  String ep  = params["endpoint"].as<String>();
  String ip  = params["assigned_ip"].as<String>();
  bool ok = _wg->up(srv, ep, ip);
  if (!ok) {
    return "wireguard up() failed";
  }
  return String("tunnel up, assigned=") + _wg->assignedIp();
}

String MothershipService::actionCloseTunnel(JsonObjectConst params) {
  // Phase 3 — bring the tunnel back down. Sent by mothership after
  // operator session idle timeout, or on explicit close. Idempotent:
  // a closeTunnel when already down just acks.
  (void)params;
  if (!_wg) {
    return "no wireguard provider — install WireGuardModule";
  }
  _wg->down();
  return "tunnel down";
}

String MothershipService::actionSetConfig(JsonObjectConst params) {
  // Generic per-key config push. Useful for diagnostic in the field
  // — operator can flip a debug flag without rebuilding firmware.
  // Phase 2.4 wires it into a generic config-write path; for now
  // log the requested key/value and skip apply.
  String key = params["key"].as<String>();
  String val = params["value"].as<String>();
  Serial.printf("[mship.action.setConfig] %s = %s (skipped)\n",
                key.c_str(), val.c_str());
  return "ack-only";
}

String MothershipService::actionReboot(JsonObjectConst params) {
  // Schedule a deferred reboot so we can return the response first.
  // 2-second delay gives the HTTP client time to flush.
  (void)params;
  Serial.println("[mship.action.reboot] in 2s...");
  // Schedule via FreeRTOS so we don't block this task.
  xTaskCreate([](void*) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP.restart();
  }, "mship.reboot", 2048, nullptr, 1, nullptr);
  return "scheduled in 2s";
}

String MothershipService::actionLog(JsonObjectConst params) {
  // Echo a log line to Serial. Useful for "is this device alive?"
  // probes from server-side admin without needing remote shell.
  String level = params["level"].as<String>();
  String msg   = params["msg"].as<String>();
  Serial.printf("[mship.log][%s] %s\n",
                level.length() ? level.c_str() : "info",
                msg.c_str());
  return "logged";
}

int32_t MothershipService::lastCheckInAgoSec() const {
  if (_state.last_checkin_at_s == 0) return INT32_MIN;
  uint32_t now_s = (uint32_t)(millis() / 1000);
  if (now_s < _state.last_checkin_at_s) return 0;
  return (int32_t)(now_s - _state.last_checkin_at_s);
}

int32_t MothershipService::nextCheckInInSec() const {
  if (!_state.enabled) return INT32_MIN;
  if (_state.runtime_state == State::CheckingIn) return 0;
  uint32_t now_s = (uint32_t)(millis() / 1000);
  if (_state.next_checkin_at_s <= now_s) return 0;
  return (int32_t)(_state.next_checkin_at_s - now_s);
}

void MothershipService::refreshRuntimeState() {
  using S = IMothershipProvider::State;
  S newState;

  // refreshRuntimeState handles ONLY the gate cases — Disabled and
  // NeedsCert — based on inputs that can change independently of a
  // check-in (operator toggle, cert (re)load). Result-of-last-tick
  // states (CheckingIn/LastOk/LastFail) are owned by the tick lambda
  // in runCheckinLoop; we preserve whatever's there when neither
  // gate fails.
  //
  // Idle is the boot fallback when no tick has run yet.
  if (!_state.enabled) {
    newState = S::Disabled;
  } else if (!_cert || !_cert->hasValidCert()) {
    newState = S::NeedsCert;
  } else if (_state.runtime_state == S::Disabled
             || _state.runtime_state == S::NeedsCert) {
    // Gates were previously failing, now both pass → Idle until
    // the first tick lands.
    newState = S::Idle;
  } else {
    // Preserve current state — set by tick path or another refresh.
    newState = _state.runtime_state;
  }

  _state.runtime_state = newState;
  switch (newState) {
    case S::Disabled:    _state.status_label = "Disabled";    break;
    case S::NeedsCert:   _state.status_label = "NeedsCert";   break;
    case S::Idle:        _state.status_label = "Idle";        break;
    case S::CheckingIn:  _state.status_label = "CheckingIn";  break;
    case S::LastOk:      _state.status_label = "LastOk";      break;
    case S::LastFail:    _state.status_label = "LastFail";    break;
  }
}
