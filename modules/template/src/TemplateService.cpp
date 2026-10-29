#include <TemplateService.h>

#include <AdaptiveJson.h>
#include <WebManager.h>
#include <App.h>  // ESPRack::wakeLoop
#include <system/LoopWait.h>

// ===================== state <-> JSON =====================

// What lands in /config/template.json. Settings only — runtime counters
// would make every save "changed" and wear the flash.
void TemplateState::readConfig(TemplateState& s, JsonObject& root) {
  root["enabled"] = s.enabled;
  root["rx_pin"]  = s.rxPin;
  root["tx_pin"]  = s.txPin;
  root["baud"]    = s.baud;
  root["push_ms"] = s.pushMs;
}

// The single updater for the config file, the UI Save (flat object of the
// CHANGED keys only), POST /api/v1/template/state and WS writes. Absent
// keys are left alone, so a partial dict is a partial update. The
// {settings:{...}} envelope is accepted too, like the built-in modules.
StateUpdateResult TemplateState::update(JsonObject& root, TemplateState& s) {
  JsonObject src = root;
  if (root.containsKey("settings") && root["settings"].is<JsonObject>()) {
    src = root["settings"].as<JsonObject>();
  }

  bool changed = false;
  changed |= FormBuilder::updateValue(src, "enabled", s.enabled);
  changed |= FormBuilder::updateValue(src, "rx_pin",  s.rxPin);
  changed |= FormBuilder::updateValue(src, "tx_pin",  s.txPin);
  changed |= FormBuilder::updateValue(src, "baud",    s.baud);
  changed |= FormBuilder::updateValue(src, "push_ms", s.pushMs);

  // Clamp: REST and WS callers bypass the UI's min/max.
  if (s.rxPin < -1) s.rxPin = -1;
  if (s.txPin < -1) s.txPin = -1;
  if (s.baud < 300) s.baud = 300;
  if (s.pushMs < 200)   s.pushMs = 200;
  if (s.pushMs > 10000) s.pushMs = 10000;

  return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
}

// UI face. One web::Form per tab key ("status", "settings"). Runs at REST
// GET time and once at boot (buffer auto-size probe, secret-key probe on a
// default-constructed state) — keep it a pure function of `s`.
void TemplateState::buildForm(TemplateState& s, JsonObject& root) {
  web::Form st(root, "status", "UART line monitor");
  st.add(web::Text("port", s.portOpen ? "Open" : "Closed").readOnly()
             .label("Port").icon("Cable").color(s.portOpen ? "success" : "info"));
  st.add(web::Number("bytes_rx", s.bytesRx).readOnly().max(4294967295.0).format("0")
             .label("Bytes received").icon("Download"));
  st.add(web::Number("lines_rx", s.linesRx).readOnly().max(4294967295.0).format("0")
             .label("Lines received").icon("FormatListNumbered"));
  st.add(web::Number("rate_bps", s.rateBps).readOnly().max(1000000).format("0.0")
             .unit("B/s").label("Rate").icon("Speed"));
  st.add(web::Text("last_line", s.lastLine()).readOnly().label("Last line").icon("Notes"));
  // Trend: the form declares the chart; points arrive over WS as an
  // array under the same key ("rate"), see readState().
  st.add(web::Trend("rate").mode("lineChart").xAxis("timestamp")
             .line("rate_bps", "#1e88e5", "monotone").maxPoints(60).icon("ShowChart"));
  // Button bound to the manifest action registered in registerManifest().
  st.add(web::Action("a_reset").ref("template.resetCounters"));

  web::Form set(root, "settings", "Port settings");
  set.add(web::Switch("enabled", s.enabled).label("Enabled").icon("PowerSettingsNew"));
  // Number's min/max default to 0..100 and are always emitted — set them.
  set.add(web::Number("rx_pin", s.rxPin).min(-1).max(48).format("0")
              .label("RX pin (-1 = UART default)").icon("Input"));
  set.add(web::Number("tx_pin", s.txPin).min(-1).max(48).format("0")
              .label("TX pin (-1 = UART default)").icon("Output"));
  set.add(web::Dropdown("baud", s.baud)
              .option("4800", 4800).option("9600", 9600).option("19200", 19200)
              .option("38400", 38400).option("57600", 57600).option("115200", 115200)
              .label("Baud rate").icon("Speed"));
  set.add(web::Slider("push_ms", s.pushMs).min(200).max(10000).step(100)
              .label("Live push period, ms").icon("Timer"));
  set.add(web::Message("m_apply", "Saving re-opens the UART with the new pins/baud.")
              .level("info"));
}

// Machine face: flat values. Backs every WS push on the "template"
// channel AND GET /api/v1/template/state. Never put secrets here.
void TemplateState::readState(TemplateState& s, JsonObject& root) {
  root["port"]      = s.portOpen ? "Open" : "Closed";
  root["bytes_rx"]  = s.bytesRx;
  root["lines_rx"]  = s.linesRx;
  root["rate_bps"]  = s.rateBps;
  // char* (not const char*) so ArduinoJson COPIES the line: the GET /state
  // serializer may run after the service lock is released.
  if (s.linesRx) root["last_line"] = s.ring[(s.linesRx - 1) % TemplateState::kRing];
  else           root["last_line"] = "";

  JsonArray rate = root.createNestedArray("rate");   // one new trend point
  JsonObject pt  = rate.createNestedObject();
  pt["timestamp"] = s.sampleTs;
  pt["rate_bps"]  = s.rateBps;

  // Settings echo, so a second open browser tab sees another tab's save.
  root["enabled"] = s.enabled;
  root["rx_pin"]  = s.rxPin;
  root["tx_pin"]  = s.txPin;
  root["baud"]    = s.baud;
  root["push_ms"] = s.pushMs;
}

// ===================== service =====================

TemplateService::TemplateService(ConfigManager* cfgMgr, HardwareSerial& port)
    : StatefulService<TemplateState>(),
      _port(port),
      // bufferSize sizes the load/save docs AND the boot-time form probe
      // that auto-discovers web::Secret fields (formReader, last arg) for
      // encryption at rest — it must fit buildForm's output, not just the
      // small config file.
      _cfg(cfgMgr,
           "template",
           TEMPLATE_CONFIG_FILE,
           2048,
           this,
           TemplateState::readConfig,
           TemplateState::update,
           false /*autoSave — ignored, saving is explicit*/,
           nullptr /*validate*/,
           TemplateState::buildForm /*formReader*/) {
  // Runs after every PROPAGATED update (UI save, POST /state, WS write).
  // Runtime ticks use updateWithoutPropagation() and never get here.
  // No broadcastWs() needed: the feature's WS binding already pushes on
  // every propagated update.
  addUpdateHandler([this](const String& origin) {
    _cfg.saveIfChanged(origin);   // CRC-gated: no flash write if unchanged
    _applyPending = true;         // UART re-open happens in loop()
    ESPRack::wakeLoop();          // async_tcp: the loop may be waiting
  }, false);
}

void TemplateService::registerManifest(WebManager* web) {
  if (!web) return;

  // Typed feature: GET /api/v1/template/form (buildForm),
  // GET+POST /api/v1/template/state (readState / update), live WS channel
  // "template". Paths come from the id — never author them.
  _feature = web->registerFeature<TemplateState>(
      WebFeature("template")
          .title("UART monitor")
          .version("0.1.0")
          .component(WebComponent::DynamicSettings)
          .authenticated()   // read gate for form + state (and the write floor)
          .menu(WebMenu("UART monitor").icon("Cable").order(60))
          // Any non-empty value marks the feature live; the WS channel is
          // the feature id, multiplexed over the single socket.
          .ws("/ws/template")
          .tab(WebTab("status", "Status").icon("Monitor").live())
          // adminOnly() hides the tab from non-admins AND (LIB-FIX-011) makes
          // its keys admin-write on the server: POST /state and WS writes
          // touching them need an admin. Use .writeAuth()/.adminWrite() to
          // set the write level independently of visibility.
          .tab(WebTab("settings", "Settings").icon("Tune").postable().adminOnly())
          .build(),
      this,
      TemplateState::buildForm, TemplateState::update,    // form face
      TemplateState::readState, TemplateState::update,    // state/WS face
      WEBFEATURE_BUFFER_AUTO,                             // REST: measured at boot
      1024);                                              // WS frame buffer

  // Action: POST /api/v1/actions/template.resetCounters. Runs on async_tcp;
  // state is mutex-guarded so touching it here is safe.
  web->registerAction(
      WebAction("template.resetCounters", "Reset counters")
          .icon(WebIcon::Refresh).color("secondary")
          .authenticated()
          .confirm("Reset the RX counters?")
          .success("Counters reset")
          .handler([this](AsyncWebServerRequest* r) {
            updateWithoutPropagation([](TemplateState& s) {
              s.bytesRx = s.linesRx = s.bytesAtSample = 0;
              s.rateBps = 0;
              return StateUpdateResult::CHANGED;
            });
            r->send(200, "application/json", "{\"ok\":true}");
          }));

  // Custom endpoint: GET /api/v1/template/lines (recent lines, newest
  // first). A WebPageSpec owns raw endpoints so they get auth-wrapped and
  // appear in the manifest / OpenAPI. No menu.label = not in the side nav.
  WebPageSpec page;
  page.id    = "templateLines";
  page.title = "UART monitor: recent lines";
  page.auth  = WebAuthLevel::Authenticated;
  WebEndpointMeta e;
  e.method  = "GET";
  e.path    = TEMPLATE_LINES_PATH;   // literal: the pointer must outlive the spec
  e.auth    = WebAuthLevel::Authenticated;
  e.handler = [this](AsyncWebServerRequest* r) { serveLines(r); };
  page.endpoints.push_back(std::move(e));
  web->registerPage(std::move(page));
}

void TemplateService::begin() {
  // FS is mounted by now (App::begin step 1) — load or seed defaults.
  (void)_cfg.ensureLoaded();
  applyPort();
}

void TemplateService::loop() {
  // Load + store, not exchange: the service is on the heap (PSRAM on Xtensa,
  // where RMW atomics fail). A save between the two is applied right here.
  if (_applyPending.load()) {
    _applyPending.store(false);
    applyPort();
  }
  if (_open) drainPort();
  pushLive();
}

uint32_t TemplateService::loopWaitMs(uint32_t nowMs) const {
  if (_applyPending.load()) return 0;
  // The UART has no event here, so it is polled at the default cadence
  // (HardwareSerial::onReceive + ESPRack::wakeLoop() would make it one, at
  // the cost of a UART event task).
  if (_open) return ESPRACK_LOOP_DEFAULT_WAIT_MS;
  if (_feature && _feature->hasSubscribers()) return ESPRack::LoopWait::dueIn(nowMs, _lastPushMs, _pushMs);
  return ESPRACK_LOOP_IDLE_WAIT_MS;
}

void TemplateService::end() {
  if (_open) {
    _port.end();
    _open = false;
  }
}

void TemplateService::applyPort() {
  bool en = false;
  int rx = -1, tx = -1, baud = 0;
  read([&](TemplateState& s) {
    en = s.enabled; rx = s.rxPin; tx = s.txPin; baud = s.baud;
  });

  const bool want = en && baud > 0;
  if (want == _open && rx == _openRx && tx == _openTx && baud == _openBaud) return;

  if (_open) {
    _port.end();
    _open = false;
  }
  if (want) {
    // -1 keeps the UART's default pin for that direction.
    _port.begin((unsigned long)baud, SERIAL_8N1, (int8_t)rx, (int8_t)tx);
    _open = true;
    log_i("[template] UART open: baud=%d rx=%d tx=%d", baud, rx, tx);
  } else {
    log_i("[template] UART closed");
  }
  _openRx = rx; _openTx = tx; _openBaud = baud;
  _lineLen = 0;

  const bool open = _open;
  updateWithoutPropagation([open](TemplateState& s) {
    s.portOpen = open;
    return StateUpdateResult::CHANGED;
  });
}

void TemplateService::drainPort() {
  // Bounded per tick: onLoop() must never block the shared main loop.
  static constexpr int kBudget = 256;
  uint32_t newBytes = 0;
  for (int i = 0; i < kBudget && _port.available() > 0; ++i) {
    const int c = _port.read();
    if (c < 0) break;
    ++newBytes;
    if (c == '\n' || c == '\r') {
      if (_lineLen == 0) continue;
      _line[_lineLen] = '\0';
      _lineLen = 0;
      updateWithoutPropagation([this](TemplateState& s) {
        strlcpy(s.ring[s.linesRx % TemplateState::kRing], _line, TemplateState::kLineLen);
        ++s.linesRx;
        return StateUpdateResult::CHANGED;
      });
    } else if (_lineLen < sizeof(_line) - 1) {
      _line[_lineLen++] = (char)c;
    }
  }
  if (newBytes) {
    updateWithoutPropagation([newBytes](TemplateState& s) {
      s.bytesRx += newBytes;
      return StateUpdateResult::CHANGED;
    });
  }
}

void TemplateService::pushLive() {
  // Zero work while nobody has the feature open.
  if (!_feature || !_feature->hasSubscribers()) return;

  uint16_t pushMs = 1000;
  read([&](TemplateState& s) { pushMs = s.pushMs; });
  _pushMs = pushMs;
  const uint32_t now = millis();
  const uint32_t dt  = now - _lastPushMs;
  if (dt < pushMs) return;
  _lastPushMs = now;

  updateWithoutPropagation([now, dt](TemplateState& s) {
    s.rateBps       = dt ? (s.bytesRx - s.bytesAtSample) * 1000.0f / dt : 0.0f;
    s.bytesAtSample = s.bytesRx;
    s.sampleTs      = now;
    return StateUpdateResult::CHANGED;
  });
  _feature->broadcastWs("tick");
}

void TemplateService::serveLines(AsyncWebServerRequest* r) {
  // sendAdaptiveJson sizes the doc to the payload and sheds a retryable
  // 503 under heap pressure. The fill lambda may run more than once.
  sendAdaptiveJson(r, [this](JsonObject& out) {
    read([&](TemplateState& s) {
      out["total"] = s.linesRx;
      JsonArray lines = out.createNestedArray("lines");
      const uint32_t n = s.linesRx < TemplateState::kRing ? s.linesRx : TemplateState::kRing;
      for (uint32_t i = 0; i < n; ++i) {
        // char* (not const char*) => ArduinoJson COPIES it into the doc,
        // so the response never points into state after the lock drops.
        lines.add(s.ring[(s.linesRx - 1 - i) % TemplateState::kRing]);
      }
    });
  }, 768);
}
