#include <APSettingsService.h>
#include <WebManager.h>

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
  JsonArray status = FormBuilder::createForm(root, "status", "Status");
  FormBuilder::addTextField(status, "status",      AF::R,
                            APSettings::statusLabel(st),
                            icon("SettingsInputAntenna"),
                            color(APSettings::statusColor(st)));
  FormBuilder::addTextField(status, "ip_address",  AF::R,
                            WiFi.softAPIP().toString().c_str(), icon("Router"));
  FormBuilder::addTextField(status, "mac_address", AF::R,
                            WiFi.softAPmacAddress().c_str(), icon("DeviceHub"));
  FormBuilder::addNumberField(status, "station_num", AF::R,
                              (uint32_t)WiFi.softAPgetStationNum(),
                              format("0"), icon("Computer"));

  // SETTINGS
  JsonArray set = FormBuilder::createForm(root, "settings", "Settings");
  FormBuilder::addDropdownField(set, "provision_mode", AF::RW, (int)s.provisionMode,
                                opt("Always", AP_MODE_ALWAYS),
                                opt("When WiFi Disconnected", AP_MODE_DISCONNECTED),
                                opt("Never", AP_MODE_NEVER),
                                icon("SettingsInputAntenna"));
  FormBuilder::addTextField (set, "ssid",     AF::RW, s.ssid.c_str(), icon("Wifi"));
  FormBuilder::addSecretField(set, "pwd", AF::RW, s.password.c_str(),
                              icon("Lock"));
  FormBuilder::addDropdownField(set, "channel", AF::RW, (int)s.channel,
                                opt("1", 1),  opt("2", 2),  opt("3", 3),
                                opt("4", 4),  opt("5", 5),  opt("6", 6),
                                opt("7", 7),  opt("8", 8),  opt("9", 9),
                                opt("10", 10), opt("11", 11), opt("12", 12), opt("13", 13),
                                icon("Wifi"));
  FormBuilder::addSwitchField(set, "ssid_hidden", AF::RW, s.ssidHidden,
                              icon("Visibility"));
  FormBuilder::addDropdownField(set, "max_clients", AF::RW, (int)s.maxClients,
                                opt("1", 1), opt("2", 2), opt("3", 3), opt("4", 4),
                                opt("5", 5), opt("6", 6), opt("7", 7), opt("8", 8),
                                icon("Computer"));
  FormBuilder::addIPAddressField(set, "local_ip",    AF::RW, s.localIP.toString().c_str(),
                                 icon("Router"));
  FormBuilder::addIPAddressField(set, "gateway_ip",  AF::RW, s.gatewayIP.toString().c_str(),
                                 icon("Router"));
  FormBuilder::addIPAddressField(set, "subnet_mask", AF::RW, s.subnetMask.toString().c_str(),
                                 icon("Language"));
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
  if (src.containsKey("ssid"))        n.ssid        = src["ssid"].as<String>();
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

  WebFeatureSpec spec;
  spec.id         = "ap";
  spec.title      = "Access Point";
  spec.component  = "DynamicSettings";
  spec.menu.label = "Access Point";
  spec.menu.icon  = "SettingsInputAntenna";
  spec.menu.order = 150;
  spec.menu.auth  = WebAuthLevel::Authenticated;
  spec.auth       = WebAuthLevel::Authenticated;
  spec.restRead   = AP_FORM_PATH;
  spec.restUpdate = AP_FORM_PATH;
  // No wsPath — the status tab is plain REST (user explicitly asked for
  // REST-only; the tab renders as read-only DynamicSettings instead of the
  // WS-dependent DynamicStatus).

  WebTabSpec statusTab;
  statusTab.key      = "status";
  statusTab.title    = "Status";
  statusTab.restPath = AP_FORM_PATH;
  statusTab.postable = false;
  statusTab.live     = false;
  statusTab.auth     = WebAuthLevel::Authenticated;
  spec.tabs.push_back(statusTab);

  WebTabSpec settingsTab;
  settingsTab.key      = "settings";
  settingsTab.title    = "Settings";
  settingsTab.restPath = AP_FORM_PATH;
  settingsTab.postable = true;
  settingsTab.auth     = WebAuthLevel::Admin;
  spec.tabs.push_back(settingsTab);

  _feature = web->registerFeature<APSettings>(
      std::move(spec), this,
      APSettings::buildForm, APSettings::update,
      4096);
}

void APSettingsService::begin() {
  (void)_cfg.ensureLoaded();

  if (_state.ssid.length() == 0) {
    _state.ssid = SettingValue::format(FACTORY_AP_SSID);
  }
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
  Serial.println(F("Starting software access point"));
  WiFi.softAPConfig(_state.localIP, _state.gatewayIP, _state.subnetMask);
  WiFi.softAP(_state.ssid.c_str(), _state.password.c_str(), _state.channel, _state.ssidHidden, _state.maxClients);
  if (!_dnsServer) {
    IPAddress apIp = WiFi.softAPIP();
    Serial.print(F("Starting captive portal on "));
    Serial.println(apIp);
    _dnsServer = new DNSServer;
    _dnsServer->start(DNS_PORT, "*", apIp);
  }
}

void APSettingsService::stopAP() {
  if (_dnsServer) {
    Serial.println(F("Stopping captive portal"));
    _dnsServer->stop();
    delete _dnsServer;
    _dnsServer = nullptr;
  }
  Serial.println(F("Stopping software access point"));
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
