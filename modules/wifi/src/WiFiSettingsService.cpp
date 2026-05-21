#include <WiFiSettingsService.h>
#include <WebManager.h>
#include <DeviceIdentity.h>

// File-scope snapshot of the service's dual-AP selection. buildForm / wsRead
// live on WiFiSettings (no service pointer), so the service writes here and
// both readers read. Plain uint8_t access is atomic on our targets.
static volatile uint8_t s_activeAp = 0;

// -------- Runtime status helpers (stateless) --------
static const char* connStatusLabel(wl_status_t st) {
  switch (st) {
    case WL_CONNECTED:        return "Connected";
    case WL_IDLE_STATUS:
    case WL_DISCONNECTED:     return "Connecting";
    case WL_NO_SSID_AVAIL:    return "SSID not available";
    case WL_CONNECT_FAILED:   return "Connect failed";
    case WL_CONNECTION_LOST:  return "Connection lost";
    case WL_NO_SHIELD:        return "WiFi off";
    default:                  return "Disconnected";
  }
}

static const char* connStatusColor(wl_status_t st) {
  switch (st) {
    case WL_CONNECTED:        return "success";
    case WL_IDLE_STATUS:
    case WL_DISCONNECTED:     return "warning";
    case WL_CONNECT_FAILED:
    case WL_CONNECTION_LOST:  return "error";
    default:                  return "info";
  }
}

static const char* encryptionLabel(uint8_t t) {
#ifdef ESP32
  switch (t) {
    case WIFI_AUTH_OPEN:            return "Open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Ent";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "—";
  }
#else
  (void)t;
  return "—";
#endif
}

// Human-readable label for the current scan phase — emitted both in REST
// (Scan tab buildForm) and over WS so the user sees progress textually.
static String scanPhaseLabel(int n) {
  if (n == -2) return String("Idle");
  if (n == -1) return String("Scanning…");
  String out = "Found ";
  out += n;
  out += (n == 1) ? " network" : " networks";
  return out;
}

// Joins dns_ip_1 and dns_ip_2 into one "1.2.3.4,5.6.7.8" string for the
// Status tab, matching the legacy single-row DNS display.
static String joinDns(bool connected) {
  if (!connected) return String("");
  IPAddress d1 = WiFi.dnsIP(0);
  IPAddress d2 = WiFi.dnsIP(1);
  String out;
  if (IPUtils::isSet(d1)) out = d1.toString();
  if (IPUtils::isSet(d2)) { if (out.length()) out += ","; out += d2.toString(); }
  return out;
}

/* ================= FORM ================= */
void WiFiSettings::buildForm(WiFiSettings& s, JsonObject& root) {
  const wl_status_t wst = WiFi.status();
  const bool connected = (wst == WL_CONNECTED);

  // ---------- STATUS tab — 8 rows (was 12), DNS joined, live Avatar color ----------
  JsonArray status = FormBuilder::createForm(root, "status", "Status");

  // Live-color map: each status-label transitions to a palette key. When WS
  // pushes a new label the Avatar background follows in real time.
  FormBuilder::addTextField(status, "status", AF::R,
                            connStatusLabel(wst),
                            icon("Wifi"),
                            colorMap("Connected:success,"
                                     "Connecting:warning,"
                                     "SSID not available:warning,"
                                     "Connect failed:error,"
                                     "Connection lost:error,"
                                     "WiFi off:info,"
                                     "Disconnected:info,"
                                     "default:info"));

  const char* activeAp = connected ? (s_activeAp == 1 ? "Secondary" : "Primary") : "—";
  FormBuilder::addTextField(status, "active_ap", AF::R,
                            activeAp,
                            label("Active AP"), icon("SwapVerticalCircle"));

  const String ssidLive = connected ? WiFi.SSID() : String("");
  FormBuilder::addTextField(status, "ssid", AF::R, ssidLive.c_str(),
                            label("SSID"), icon("SettingsInputAntenna"));

  const String localIp = connected ? WiFi.localIP().toString() : String("");
  FormBuilder::addTextField(status, "local_ip", AF::R, localIp.c_str(),
                            label("Local IP"), icon("Router"));

  // Same source as the cert CN / mothership deviceId / AutoUpdate
  // did= — colon-separated upper-case (matches the legacy
  // WiFi.macAddress() string format so this is a visual drop-in).
  const String mac = DeviceIdentity::macColon();
  FormBuilder::addTextField(status, "mac_address", AF::R, mac.c_str(),
                            label("MAC address"), icon("DeviceHub"));

  const String subnetMask = connected ? WiFi.subnetMask().toString() : String("");
  FormBuilder::addTextField(status, "subnet_mask", AF::R, subnetMask.c_str(),
                            label("Subnet mask"), icon("Language"));

  const String gatewayIp = connected ? WiFi.gatewayIP().toString() : String("none");
  FormBuilder::addTextField(status, "gateway_ip", AF::R, gatewayIp.c_str(),
                            label("Gateway IP"), icon("Router"));

  String dnsJoined = joinDns(connected);
  FormBuilder::addTextField(status, "dns_servers", AF::R,
                            dnsJoined.length() ? dnsJoined.c_str() : "none",
                            label("DNS servers"), icon("Dns"));

  // ---------- SCAN tab ----------
  // Lazy (user-triggered) scan — no auto-kick on tab open. scanComplete()
  // returns -2 until the user presses the button; phase label surfaces the
  // state so the empty card list isn't confusing.
  JsonArray scan = FormBuilder::createForm(root, "scan", "Network Scanner");

  // Networks appear AT THE TOP — found APs surface above the status/button
  // so the user's eye lands on them immediately when a scan completes.
  // Card list carries hideAvatar() so the "Networks / no items" header is
  // suppressed when the list is empty — the Scan button + status row
  // below already communicate state. Rows are baked into the REST response
  // on every GET (no WS on this feature), so the auto-refetch triggered
  // by the Scan action surfaces the fresh list without a tab reopen.
  // loadingIf() shows a CircularProgress spinner while scan_status is
  // still "Scanning…".
  JsonObject tbl = FormBuilder::addTableField(
      scan, "networks", AF::R,
      col("ssid", "SSID", "text"),
      col("rssi", "RSSI", "number", format("0")),
      col("channel", "Ch", "number", format("0")),
      col("security", "Security", "text"),
      icon("Wifi"),
      layout("cards"),
      primaryField("ssid"),
      secondaryTemplate("Security: {security}, Ch: {channel}"),
      avatarFromField("avatar_icon"),
      badgeField("rssi", "dBm", "Wifi"),
      rowFill("ssid=ssid,pwd="),
      // Spec syntax is `<target>=<col>` — i.e. "set form field
      // <target> from the row's <col>". Fallback writes the scanned
      // row's `ssid` column into the SECONDARY form field `ssid_2`
      // (and clears the secondary `pwd_2`).
      rowFillFallback("ssid_2=ssid,pwd_2="),
      rowNav("../settings"),
      hideAvatar(),
      loadingIf("scan_status:Scanning"));

  const int scanN = WiFi.scanComplete();
  if (scanN > 0) {
    JsonArray arr = tbl["networks"].as<JsonArray>();
    for (int i = 0; i < scanN; ++i) {
      JsonObject row = arr.createNestedObject();
      row["ssid"]    = WiFi.SSID(i);
      row["rssi"]    = WiFi.RSSI(i);
      row["channel"] = WiFi.channel(i);
#ifdef ESP32
      uint8_t enc = (uint8_t)WiFi.encryptionType(i);
      row["security"]    = encryptionLabel(enc);
      row["avatar_icon"] = (enc == WIFI_AUTH_OPEN) ? "LockOpen" : "Lock";
#else
      row["security"]    = "";
      row["avatar_icon"] = "Lock";
#endif
    }
  }

  const String phase = scanPhaseLabel(scanN);
  FormBuilder::addTextField(scan, "scan_status", AF::R, phase.c_str(),
                            icon("Refresh"));

  // Scan trigger — posts to /rest/wifiScan. refetchForm tells the frontend
  // to poll /rest/wifiForm on a short schedule after the action fires so
  // the networks table above updates automatically as soon as the radio
  // surfaces results.
  FormBuilder::addActionField(scan, "scan_btn", "Scan WiFi", AF::RW,
                              actionRef("wifi.scan"), icon("Refresh"),
                              color("secondary"), refetchForm());

  // ---------- SETTINGS tab ----------
  // Three cards: Primary credentials, Backup credentials, Advanced
  // (hostname + static IP block). Wrapping the advanced section in a
  // card visually unifies it with the credentials section so the tab
  // doesn't trail off into bare list items.
  JsonArray set = FormBuilder::createForm(root, "settings", "Settings");

  // Card 1 — Primary network. Clearing writes "" to both fields so a
  // single tap wipes the slot before the user saves.
  FormBuilder::addTextField(set, "ssid", AF::RW, s.ssid.c_str(),
                            label("SSID"), icon("Wifi"),
                            card("wifi_primary"),
                            cardTitle("Primary network"),
                            cardDeletable());
  FormBuilder::addSecretField(set, "pwd", AF::RW, s.password.c_str(),
                              label("Password"), icon("Lock"),
                              card("wifi_primary"));

  // Card 2 — Backup network. Same shape as primary; left blank disables
  // the slot entirely (failover code skips empty rows).
  FormBuilder::addTextField(set, "ssid_2", AF::RW, s.ssid2.c_str(),
                            label("SSID"), icon("Wifi"),
                            card("wifi_secondary"),
                            cardTitle("Backup network"),
                            cardDeletable());
  FormBuilder::addSecretField(set, "pwd_2", AF::RW, s.password2.c_str(),
                              label("Password"), icon("Lock"),
                              card("wifi_secondary"));

  // Card 3 — Advanced. Hostname + static IP toggle and its dependent IP
  // fields. Static-IP block renders only when the switch is on.
  FormBuilder::addTextField(set, "hostname", AF::RW, s.hostname.c_str(),
                            label("Hostname"), icon("DeviceHub"),
                            card("wifi_advanced"),
                            cardTitle("Advanced"));
  FormBuilder::addSwitchField(set, "static_ip_config", AF::RW, s.staticIPConfig,
                              label("Static IP"), icon("Router"),
                              card("wifi_advanced"));
  FormBuilder::addIPAddressField(set, "local_ip",    AF::RW, s.localIP.toString().c_str(),
                                 label("Local IP"), icon("Router"), card("wifi_advanced"),
                                 showIf("static_ip_config", "true"));
  FormBuilder::addIPAddressField(set, "gateway_ip",  AF::RW, s.gatewayIP.toString().c_str(),
                                 label("Gateway IP"), icon("Router"), card("wifi_advanced"),
                                 showIf("static_ip_config", "true"));
  FormBuilder::addIPAddressField(set, "subnet_mask", AF::RW, s.subnetMask.toString().c_str(),
                                 label("Subnet mask"), icon("Language"), card("wifi_advanced"),
                                 showIf("static_ip_config", "true"));
  FormBuilder::addIPAddressField(set, "dns_ip_1",    AF::RW, s.dnsIP1.toString().c_str(),
                                 label("DNS server 1"), icon("Dns"), card("wifi_advanced"),
                                 showIf("static_ip_config", "true"));
  FormBuilder::addIPAddressField(set, "dns_ip_2",    AF::RW, s.dnsIP2.toString().c_str(),
                                 label("DNS server 2"), icon("Dns"), card("wifi_advanced"),
                                 showIf("static_ip_config", "true"));
}

/* ================= UPDATE ================= */
StateUpdateResult WiFiSettings::update(JsonObject& root, WiFiSettings& s) {
  // Accept either the raw settings dict or a DynamicSettings envelope.
  JsonObject src = root;
  if (root.containsKey("settings") && root["settings"].is<JsonObject>()) {
    src = root["settings"].as<JsonObject>();
  }

  WiFiSettings n = s;

  if (src.containsKey("ssid"))    n.ssid      = src["ssid"].as<String>();
  if (src.containsKey("pwd"))     n.password  = src["pwd"].as<String>();
  if (src.containsKey("ssid_2"))  n.ssid2     = src["ssid_2"].as<String>();
  if (src.containsKey("pwd_2"))   n.password2 = src["pwd_2"].as<String>();
  if (src.containsKey("hostname"))   n.hostname = src["hostname"].as<String>();
  if (src.containsKey("static_ip_config")) {
    n.staticIPConfig = src["static_ip_config"] | false;
  }

  JsonUtils::readIP(src, "local_ip",    n.localIP);
  JsonUtils::readIP(src, "gateway_ip",  n.gatewayIP);
  JsonUtils::readIP(src, "subnet_mask", n.subnetMask);
  JsonUtils::readIP(src, "dns_ip_1",    n.dnsIP1);
  JsonUtils::readIP(src, "dns_ip_2",    n.dnsIP2);

  // Swap DNS slots if 2 is populated but 1 is not.
  if (IPUtils::isNotSet(n.dnsIP1) && IPUtils::isSet(n.dnsIP2)) {
    n.dnsIP1 = n.dnsIP2;
    n.dnsIP2 = INADDR_NONE;
  }
  // If static-IP is requested but any of the three core addresses is missing,
  // fall back to DHCP instead of silently half-configuring the radio.
  if (n.staticIPConfig && (IPUtils::isNotSet(n.localIP) ||
                           IPUtils::isNotSet(n.gatewayIP) ||
                           IPUtils::isNotSet(n.subnetMask))) {
    n.staticIPConfig = false;
  }

  // Resolve hostname default once on very first save.
  if (n.hostname.length() == 0) {
    n.hostname = SettingValue::format(FACTORY_WIFI_HOSTNAME);
  }

  if (n == s) return StateUpdateResult::UNCHANGED;
  s = n;
  return StateUpdateResult::CHANGED;
}

/* ================= SERVICE ================= */
WiFiSettingsService::WiFiSettingsService(ConfigManager* cfgMgr)
    : StatefulService<WiFiSettings>(),
      _cfg(cfgMgr,
           "wifi",
           WIFI_SETTINGS_FILE,
           2048,
           this,
           WiFiSettings::readConfig,
           WiFiSettings::update,
           false /*autoSave*/,
           nullptr /*validator*/,
           WiFiSettings::buildForm /*formReader — drives auto-discovery of secret keys*/) {
  // Radio comes up fresh: mirror the legacy init sequence which toggles
  // through MAX/NULL to force a clean driver state.
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.mode(WIFI_OFF);
  }
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  // Reset radio state. The historical "MAX → NULL" double-set was a
  // 2.x Arduino-core trick to force-clear the driver; on Arduino 3.x /
  // IDF 5+ (ESP32-C6) WIFI_MODE_MAX is an enum sentinel, not a valid
  // mode — esp_wifi_set_mode() returns ESP_ERR_INVALID_ARG and the
  // assert kills the chip before Serial.begin() runs (silent reboot
  // loop, only ROM bootloader output visible). NULL alone is enough.
  WiFi.mode(WIFI_MODE_NULL);

  // Disable WiFi modem sleep — keeps the RX path awake between beacons
  // so concurrent AP clients see steady-state behaviour while the STA
  // side is scanning or reconnecting. Small ~20 mA extra draw that the
  // device can afford; eliminates a class of flaky disconnects users
  // see when the modem power-saves between tasks.
#ifdef ESP32
  WiFi.setSleep(false);
#endif

#ifdef ESP32
  WiFi.onEvent(std::bind(&WiFiSettingsService::onStationModeConnected, this,
                         std::placeholders::_1, std::placeholders::_2),
               WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_CONNECTED);
  WiFi.onEvent(std::bind(&WiFiSettingsService::onStationModeDisconnected, this,
                         std::placeholders::_1, std::placeholders::_2),
               WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent(std::bind(&WiFiSettingsService::onStationModeGotIP, this,
                         std::placeholders::_1, std::placeholders::_2),
               WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(std::bind(&WiFiSettingsService::onStationModeStop, this,
                         std::placeholders::_1, std::placeholders::_2),
               WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_STOP);
#endif

  // Defer FS write + WiFi.disconnect to the main loop task so the async
  // HTTP response returns fast. Running these inline on async_tcp has the
  // POST handler busy for 100–500 ms while LittleFS commits, which the
  // browser experiences as a sluggish Save.
  addUpdateHandler([this](const String& origin) {
    _pendingSaveOrigin = origin;
    _pendingSave = true;
    _pendingReconfigure = true;
  }, false);
}

void WiFiSettingsService::registerManifest(WebManager* web) {
  if (!web) return;

  // Scan trigger action — referenced from the Scan tab's action button via
  // actionRef("wifi.scan"). Registered BEFORE the feature so actionRef
  // resolves when the manifest is built.
  {
    WebActionSpec scan;
    scan.id             = "wifi.scan";
    scan.title          = "Scan WiFi";
    scan.icon           = "Refresh";
    scan.color          = "secondary";
    scan.restPath       = WIFI_SCAN_KICK_PATH;
    scan.method         = "POST";
    scan.auth           = WebAuthLevel::Admin;
    scan.successMessage = "Scanning WiFi networks…";
    scan.handler        = [this](AsyncWebServerRequest* r) { this->kickScan(r); };
    web->registerAction(scan);
  }

  WebFeatureSpec spec;
  spec.id         = "wifi";
  spec.title      = "WiFi Connection";
  spec.component  = "DynamicSettings";
  spec.menu.label = "WiFi Connection";
  spec.menu.icon  = "Wifi";
  spec.menu.order = 140;
  spec.menu.auth  = WebAuthLevel::Authenticated;
  spec.auth       = WebAuthLevel::Authenticated;
  spec.restRead   = WIFI_FORM_PATH;
  spec.restUpdate = WIFI_FORM_PATH;
  // REST-only feature — no WS binding. Status/scan refresh on tab switch
  // or manual page reload, matching the AP feature style and keeping the
  // WebSocket channel quiet for this section entirely.

  WebTabSpec statusTab;
  statusTab.key      = "status";
  statusTab.title    = "WiFi Status";
  statusTab.restPath = WIFI_FORM_PATH;
  statusTab.postable = false;
  statusTab.live     = false;
  statusTab.auth     = WebAuthLevel::Authenticated;
  spec.tabs.push_back(statusTab);

  WebTabSpec scanTab;
  scanTab.key      = "scan";
  scanTab.title    = "Scan Networks";
  scanTab.restPath = WIFI_FORM_PATH;
  scanTab.postable = false;
  scanTab.live     = false;
  scanTab.auth     = WebAuthLevel::Admin;
  spec.tabs.push_back(scanTab);

  WebTabSpec settingsTab;
  settingsTab.key      = "settings";
  settingsTab.title    = "WiFi Settings";
  settingsTab.restPath = WIFI_FORM_PATH;
  settingsTab.postable = true;
  settingsTab.live     = false;
  settingsTab.auth     = WebAuthLevel::Admin;
  spec.tabs.push_back(settingsTab);

  // REST-only registration: single buffer shared by GET + POST handlers.
  // 16 KB leaves comfortable headroom for the full {status,scan,settings}
  // envelope plus up to ~20 scan rows.
  _feature = web->registerFeature<WiFiSettings>(
      std::move(spec), this,
      WiFiSettings::buildForm, WiFiSettings::update,
      16384);
}

void WiFiSettingsService::begin() {
  (void)_cfg.ensureLoaded();

  if (_state.hostname.length() == 0) {
    _state.hostname = SettingValue::format(FACTORY_WIFI_HOSTNAME);
  }

  // Pre-select the slot whose SSID is non-empty, preferring primary.
  _activeAp = (_state.ssid.length() == 0 && _state.ssid2.length() > 0) ? 1 : 0;
  s_activeAp = _activeAp;
  _apAttempts = 0;

  reconfigureWiFiConnection();
}

void WiFiSettingsService::loop() {
  unsigned long now = millis();

  // Deferred work from the POST update handler — executed here on the main
  // loop task so the async HTTP handler can return promptly. Reconfigure
  // first (resets the backoff so the next manageSTA fires with fresh
  // credentials) and THEN persist, so a reboot mid-save still leaves a
  // consistent config on flash.
  if (_pendingReconfigure) {
    _pendingReconfigure = false;
    reconfigureWiFiConnection();
  }
  if (_pendingSave) {
    _pendingSave = false;
    bool ok = _cfg.saveIfChanged(_pendingSaveOrigin);
    Serial.printf_P(PSTR("[wifi] saveIfChanged(%s) -> %d\r\n"),
                    _pendingSaveOrigin.c_str(), (int)ok);
  }

  if (!_lastConnectionAttempt ||
      (unsigned long)(now - _lastConnectionAttempt) >= WIFI_RECONNECTION_DELAY) {
    _lastConnectionAttempt = now;
    manageSTA();
  }

  // Clear the scan-in-progress guard once the radio produces results so
  // manageSTA can resume on the next cycle. No broadcast — this feature is
  // REST-only; status/scan tabs pick up changes on the next GET.
  if (_scanInProgress) {
    int st = WiFi.scanComplete();
    if (st >= 0) {
      Serial.printf_P(PSTR("[wifi] scan complete: %d networks, freeHeap=%u\r\n"),
                      st, (unsigned)ESP.getFreeHeap());
      setScanInProgress(false);
    }
  }

  // Keep the static snapshot of the active slot in sync for buildForm.
  s_activeAp = _activeAp;
}

void WiFiSettingsService::reconfigureWiFiConnection() {
  // If a scan is in flight, defer the reconfigure — yanking the radio
  // mid-scan leaves the driver in a confused state and produces a string
  // of ERR_NETWORK_CHANGED / reason-2 disconnects. The flag stays set;
  // the NEXT loop iteration after the scan settles will pick it up.
  if (isScanInProgress()) {
    Serial.println(F("[wifi] reconfigureWiFiConnection deferred — scan in progress"));
    _pendingReconfigure = true;
    return;
  }

  // Force the next loop() iteration to (re)begin immediately.
  _lastConnectionAttempt = 0;
  // Any settings change resets failover bookkeeping: we re-evaluate slot
  // preference from scratch based on what the user just saved.
  _activeAp = (_state.ssid.length() == 0 && _state.ssid2.length() > 0) ? 1 : 0;
  s_activeAp = _activeAp;
  _apAttempts = 0;

#ifdef ESP32
  WiFi.disconnect(false, true);
  _stopping = false;
#elif defined(ESP8266)
  WiFi.disconnect(false);
#endif
}

void WiFiSettingsService::pickActiveCreds(String& outSsid, String& outPass) {
  if (_activeAp == 1 && _state.ssid2.length() > 0) {
    outSsid = _state.ssid2; outPass = _state.password2; return;
  }
  if (_activeAp == 0 && _state.ssid.length() > 0) {
    outSsid = _state.ssid;  outPass = _state.password;  return;
  }
  // Selected slot empty — flip to the other and persist the choice so the
  // rest of the loop (status display, event handlers) stays consistent.
  _activeAp = (_activeAp == 0) ? 1 : 0;
  s_activeAp = _activeAp;
  if (_activeAp == 1) {
    outSsid = _state.ssid2; outPass = _state.password2;
  } else {
    outSsid = _state.ssid;  outPass = _state.password;
  }
}

void WiFiSettingsService::manageSTA() {
  if (WiFi.isConnected()) return;
  if (isScanInProgress()) {
    Serial.println(F("[wifi] manageSTA skipped — scan in progress"));
    return;
  }

  String ssid, password;
  pickActiveCreds(ssid, password);
  if (ssid.length() == 0) return;  // nothing configured — nothing to do

  ensureStaEnabledNoKillAp();

  wl_status_t st = WiFi.status();
  if (st == WL_IDLE_STATUS) return;  // already attempting

  Serial.printf_P(PSTR("[wifi] Connecting to WiFi (%s slot, ssid='%s', static=%d)\r\n"),
                  _activeAp == 1 ? "secondary" : "primary", ssid.c_str(),
                  (int)_state.staticIPConfig);

  if (_state.staticIPConfig) {
    WiFi.config(_state.localIP, _state.gatewayIP, _state.subnetMask,
                _state.dnsIP1, _state.dnsIP2);
  } else {
#ifdef ESP32
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    WiFi.setHostname(_state.hostname.c_str());
#elif defined(ESP8266)
    WiFi.config(INADDR_ANY, INADDR_ANY, INADDR_ANY);
    WiFi.hostname(_state.hostname);
#endif
  }
  WiFi.begin(ssid.c_str(), password.c_str());
}

void WiFiSettingsService::ensureStaEnabledNoKillAp() {
#ifdef ESP32
  wifi_mode_t mode = (wifi_mode_t)WiFi.getMode();
  if ((mode & WIFI_STA) != 0) return;
  if ((mode & WIFI_AP) != 0)       WiFi.mode(WIFI_AP_STA);
  else if (mode == WIFI_OFF)       WiFi.mode(WIFI_STA);
  else                             WiFi.mode(WIFI_STA);
#elif defined(ESP8266)
  if (WiFi.getMode() != WIFI_STA && WiFi.getMode() != WIFI_AP_STA) {
    WiFi.mode(WIFI_AP_STA);
  }
#endif
}

#ifdef ESP32
void WiFiSettingsService::onStationModeConnected(WiFiEvent_t, WiFiEventInfo_t info) {
  Serial.printf_P(PSTR("[wifi] event: connected, channel=%u\r\n"),
                  (unsigned)info.wifi_sta_connected.channel);
  _apAttempts = 0;  // lock in the current slot as "working"
}

void WiFiSettingsService::onStationModeDisconnected(WiFiEvent_t, WiFiEventInfo_t info) {
  Serial.printf_P(PSTR("[wifi] event: disconnected, reason=%u, attempts=%u\r\n"),
                  (unsigned)info.wifi_sta_disconnected.reason,
                  (unsigned)(_apAttempts + 1));

  // Failover: after N consecutive misses on the active slot, try the other
  // one (only if secondary is configured — otherwise keep hammering primary).
  if (++_apAttempts >= WIFI_AP_SWAP_AFTER_FAILS) {
    const bool hasPrimary   = _state.ssid.length()  > 0;
    const bool hasSecondary = _state.ssid2.length() > 0;
    if (hasPrimary && hasSecondary) {
      _activeAp = (_activeAp == 0) ? 1 : 0;
      s_activeAp = _activeAp;
      _apAttempts = 0;
      Serial.printf_P(PSTR("WiFi failover → %s slot\r\n"),
                      _activeAp == 1 ? "secondary" : "primary");
    } else {
      _apAttempts = 0;  // keep the slot; reset counter so we don't overflow
    }
  }

  // Do NOT reset _lastConnectionAttempt here: the 30 s backoff must apply
  // so we don't spin in a tight reconnect loop when no AP is in range.
  WiFi.disconnect(false, false);
}

void WiFiSettingsService::onStationModeGotIP(WiFiEvent_t, WiFiEventInfo_t) {
  Serial.printf_P(PSTR("[wifi] event: got IP %s host=%s rssi=%d ch=%u\r\n"),
                  WiFi.localIP().toString().c_str(), WiFi.getHostname(),
                  (int)WiFi.RSSI(), (unsigned)WiFi.channel());
}

void WiFiSettingsService::onStationModeStop(WiFiEvent_t, WiFiEventInfo_t) {
  if (_stopping) {
    _lastConnectionAttempt = 0;
    _stopping = false;
  }
}
#endif

void WiFiSettingsService::kickScan(AsyncWebServerRequest* request) {
  // Idempotent: if a scan is already running (scanComplete()==-1) we don't
  // touch the radio. Otherwise we drop stale results and kick a fresh async
  // scan — manageSTA sees _scanInProgress and holds back any (re)begin()
  // until the scan settles. The service loop clears the flag once
  // scanComplete() returns ≥0; REST re-fetches on next tab visit surface
  // the new network list.
  int st = WiFi.scanComplete();
  Serial.printf_P(PSTR("[wifi] kickScan: prev scanComplete=%d freeHeap=%u\r\n"),
                  st, (unsigned)ESP.getFreeHeap());
  if (st != -1) {
    if (st > -1) WiFi.scanDelete();
    WiFi.scanNetworks(true);
  }
  setScanInProgress(true);
  request->send(202);
}
