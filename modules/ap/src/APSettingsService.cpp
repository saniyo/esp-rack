#include <APSettingsService.h>
#include <WebManager.h>
#include <WebFeatureBuilder.h>
#include <web/Form.h>
#include <DeviceIdentity.h>

/* ================= Static runtime emitters ================= */
static uint8_t currentAPStatus(uint8_t provisionMode) {
  WiFiMode_t mode = WiFi.getMode();
  bool apActive = mode == WIFI_AP || mode == WIFI_AP_STA;
  if (apActive && provisionMode != AP_MODE_ALWAYS && WiFi.status() == WL_CONNECTED) {
    return (uint8_t)APNetworkStatus::LINGERING;
  }
  return (uint8_t)(apActive ? APNetworkStatus::ACTIVE : APNetworkStatus::INACTIVE);
}

void APSettings::buildForm(APSettings& s, JsonObject& root) {
  uint8_t st = currentAPStatus(s.provisionMode);

  // STATUS (read-only; WS pushes keep values fresh)
  web::Form status(root, "status", "Status");
  status.add(web::Text("status", APSettings::statusLabel(st)).readOnly()
                 .icon("SettingsInputAntenna").color(APSettings::statusColor(st)));
  status.add(web::Text("ip_address", WiFi.softAPIP().toString().c_str()).readOnly().icon("Router"));
  status.add(web::Text("mac_address", WiFi.softAPmacAddress().c_str()).readOnly().icon("DeviceHub"));
  status.add(web::Number("station_num", (double)(uint32_t)WiFi.softAPgetStationNum())
                 .readOnly().format("0").icon("Computer"));

  // SETTINGS
  web::Form set(root, "settings", "Settings");
  set.add(web::Dropdown("provision_mode", (int)s.provisionMode)
              .option("Always", AP_MODE_ALWAYS)
              .option("When WiFi Disconnected", AP_MODE_DISCONNECTED)
              .option("Never", AP_MODE_NEVER)
              .icon("SettingsInputAntenna"));
  // SSID is read-only — auto-derived from DeviceIdentity at every
  // startAP() so the broadcast name stays in lock-step with the cert /
  // MQTT clientId / mothership identity. Single source of truth: no
  // operator override surface exists.
  set.add(web::Text("ssid", s.ssid.c_str()).readOnly().label("SSID (auto)").icon("Wifi"));
  set.add(web::Secret("pwd", s.password.c_str()).icon("Lock"));
  set.add(web::Dropdown("channel", (int)s.channel)
              .option("1", 1).option("2", 2).option("3", 3).option("4", 4)
              .option("5", 5).option("6", 6).option("7", 7).option("8", 8)
              .option("9", 9).option("10", 10).option("11", 11).option("12", 12)
              .option("13", 13).icon("Wifi"));
  set.add(web::Switch("ssid_hidden", s.ssidHidden).icon("Visibility"));
  set.add(web::Dropdown("max_clients", (int)s.maxClients)
              .option("1", 1).option("2", 2).option("3", 3).option("4", 4)
              .option("5", 5).option("6", 6).option("7", 7).option("8", 8)
              .icon("Computer"));
  set.add(web::Ip("local_ip", s.localIP.toString().c_str()).icon("Router"));
  set.add(web::Ip("gateway_ip", s.gatewayIP.toString().c_str()).icon("Router"));
  set.add(web::Ip("subnet_mask", s.subnetMask.toString().c_str()).icon("Language"));
}

StateUpdateResult APSettings::update(JsonObject& root, APSettings& s) {
  JsonObject src = root;
  if (root.containsKey("settings") && root["settings"].is<JsonObject>()) {
    src = root["settings"].as<JsonObject>();
  }

  APSettings n = s;

  if (src.containsKey("provision_mode")) {
    int pm = src["provision_mode"] | (int)FACTORY_AP_PROVISION_MODE;
    switch (pm) {
      case AP_MODE_ALWAYS:
      case AP_MODE_DISCONNECTED:
      case AP_MODE_NEVER: break;
      default: pm = AP_MODE_ALWAYS;
    }
    n.provisionMode = (uint8_t)pm;
  }
  // ssid deliberately ignored on POST — display-only field; runtime
  // value is set in begin() / startAP() from DeviceIdentity.
  if (src.containsKey("pwd"))         n.password    = src["pwd"].as<String>();
  if (src.containsKey("channel"))     n.channel     = (uint8_t)(src["channel"] | (int)FACTORY_AP_CHANNEL);
  if (src.containsKey("ssid_hidden")) n.ssidHidden  = src["ssid_hidden"] | (bool)FACTORY_AP_SSID_HIDDEN;
  if (src.containsKey("max_clients")) n.maxClients  = (uint8_t)(src["max_clients"] | (int)FACTORY_AP_MAX_CLIENTS);

  JsonUtils::readIP(src, "local_ip",    n.localIP,    FACTORY_AP_LOCAL_IP);
  JsonUtils::readIP(src, "gateway_ip",  n.gatewayIP,  FACTORY_AP_GATEWAY_IP);
  JsonUtils::readIP(src, "subnet_mask", n.subnetMask, FACTORY_AP_SUBNET_MASK);

  if (n == s) return StateUpdateResult::UNCHANGED;
  s = n;
  return StateUpdateResult::CHANGED;
}

/* ================= Service ================= */
APSettingsService::APSettingsService(ConfigManager* cfgMgr)
    : StatefulService<APSettings>(),
      _cfg(cfgMgr,
           "ap",
           AP_FILE,
           2048,
           this,
           APSettings::readConfig,
           APSettings::update,
           false /*autoSave*/,
           nullptr /*validator*/,
           APSettings::buildForm /*formReader*/) {
  addUpdateHandler([this](const String& origin) {
    reconfigureAP();
    _cfg.saveIfChanged(origin);
  }, false);
}

void APSettingsService::registerManifest(WebManager* web) {
  if (!web) return;

  // Built with the fluent WebFeature builder. nestUnder("wifi") is the
  // tree-menu placement directive — "drop Access Point onto the WiFi feature"
  // — referencing the WiFi feature's id by string. No wsPath: the status tab
  // is plain REST (renders read-only DynamicSettings, not WS DynamicStatus).
  auto spec = WebFeature("ap")
      .title("Access Point")
      .component(WebComponent::DynamicSettings)
      .authenticated()
      .menu(WebMenu("Access Point")
                .icon(WebIcon::SettingsInputAntenna)
                .order(150)
                .nestUnder("wifi"))          // ← nest under WiFi. Its own tabs
                                             //   (Status/Settings) expand as tree
                                             //   leaves via the global default
                                             //   (WebMenuMeta::expandTabs = true).
      .rest(AP_FORM_PATH)
      .tab(WebTab("status", "Status").rest(AP_FORM_PATH))
      .tab(WebTab("settings", "Settings").rest(AP_FORM_PATH).postable().adminOnly())
      .build();

  _feature = web->registerFeature<APSettings>(
      std::move(spec), this,
      APSettings::buildForm, APSettings::update,
      4096);
}

void APSettingsService::begin() {
  (void)_cfg.ensureLoaded();

  // ALWAYS overwrite ssid — single source of truth, recomputed from
  // DeviceIdentity on every boot regardless of any stale value an older
  // firmware may have written to disk. Pick canonical when it fits the
  // 32-octet IEEE 802.11 SSID limit, otherwise compact "<project>-<uid8>"
  // (drops the MAC segment, still uniquely identifies the device via
  // the eFuse uid).
  String canonical = DeviceIdentity::canonical();
  _state.ssid = (canonical.length() <= 32)
                    ? canonical
                    : (DeviceIdentity::projectName() + "-" + DeviceIdentity::uidHex8());

  if (!_state.localIP)    _state.localIP.fromString(FACTORY_AP_LOCAL_IP);
  if (!_state.gatewayIP)  _state.gatewayIP.fromString(FACTORY_AP_GATEWAY_IP);
  if (!_state.subnetMask) _state.subnetMask.fromString(FACTORY_AP_SUBNET_MASK);

  reconfigureAP();
}

void APSettingsService::reconfigureAP() {
  _lastManaged = millis() - MANAGE_NETWORK_DELAY;
  _reconfigureAp = true;
}

void APSettingsService::loop() {
  unsigned long currentMillis = millis();
  unsigned long manageElapsed = (unsigned long)(currentMillis - _lastManaged);
  if (manageElapsed >= MANAGE_NETWORK_DELAY) {
    _lastManaged = currentMillis;
    manageAP();
  }
  handleDNS();
}

void APSettingsService::manageAP() {
  WiFiMode_t currentWiFiMode = WiFi.getMode();
  if (_state.provisionMode == AP_MODE_ALWAYS ||
      (_state.provisionMode == AP_MODE_DISCONNECTED && WiFi.status() != WL_CONNECTED)) {
    if (_reconfigureAp || currentWiFiMode == WIFI_OFF || currentWiFiMode == WIFI_STA) {
      startAP();
    }
  } else if ((currentWiFiMode == WIFI_AP || currentWiFiMode == WIFI_AP_STA) &&
             (_reconfigureAp || !WiFi.softAPgetStationNum())) {
    stopAP();
  }
  _reconfigureAp = false;
}

void APSettingsService::startAP() {
  log_d("Starting software access point");
  WiFi.softAPConfig(_state.localIP, _state.gatewayIP, _state.subnetMask);
  WiFi.softAP(_state.ssid.c_str(), _state.password.c_str(), _state.channel, _state.ssidHidden, _state.maxClients);
  if (!_dnsServer) {
    IPAddress apIp = WiFi.softAPIP();
    log_i("[ap] starting captive portal on %s", apIp.toString().c_str());
    _dnsServer = new DNSServer;
    _dnsServer->start(DNS_PORT, "*", apIp);
  }
}

void APSettingsService::stopAP() {
  if (_dnsServer) {
    log_d("Stopping captive portal");
    _dnsServer->stop();
    delete _dnsServer;
    _dnsServer = nullptr;
  }
  log_d("Stopping software access point");
  WiFi.softAPdisconnect(true);
}

void APSettingsService::handleDNS() {
  if (_dnsServer) {
    _dnsServer->processNextRequest();
  }
}

APNetworkStatus APSettingsService::getAPNetworkStatus() {
  return (APNetworkStatus)currentAPStatus(_state.provisionMode);
}
