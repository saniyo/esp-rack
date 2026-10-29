#include <TemplateExtHwService.h>

#include <UpdateError.h>
#include <WebManager.h>
#include <App.h>  // ESPRack::wakeLoop
#include <system/LoopWait.h>

using ESPRack::HwDescriptor;
using ESPRack::HwKind;
using ESPRack::HwState;

// ===================== state <-> JSON =====================

void TemplateExtHwState::describeHw(HwDescriptor& d) const {
  d.clear();
  d.disabled = !enabled;
  d.add(HwKind::Gpio, (int16_t)outPin, "out", ESPRack::kHwOutput);
  d.add(HwKind::Gpio, (int16_t)buttonPin, "button", ESPRack::kHwOptional);
}

void TemplateExtHwState::readConfig(TemplateExtHwState& s, JsonObject& root) {
  root["enabled"]    = s.enabled;
  root["out_pin"]    = s.outPin;
  root["button_pin"] = s.buttonPin;
  root["active_low"] = s.activeLow;
  root["on"]         = s.on;
}

// The plain updater: config file load, and the base of updateChecked().
StateUpdateResult TemplateExtHwState::update(JsonObject& root, TemplateExtHwState& s) {
  JsonObject src = root;
  if (root.containsKey("settings") && root["settings"].is<JsonObject>()) {
    src = root["settings"].as<JsonObject>();
  }
  bool changed = false;
  changed |= FormBuilder::updateValue(src, "enabled",    s.enabled);
  changed |= FormBuilder::updateValue(src, "out_pin",    s.outPin);
  changed |= FormBuilder::updateValue(src, "button_pin", s.buttonPin);
  changed |= FormBuilder::updateValue(src, "active_low", s.activeLow);
  changed |= FormBuilder::updateValue(src, "on",         s.on);
  if (s.outPin < -1) s.outPin = -1;
  if (s.buttonPin < -1) s.buttonPin = -1;
  return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
}

void TemplateExtHwState::buildForm(TemplateExtHwState& s, JsonObject& root) {
  const bool ok = s.hwState == HwState::Ok;
  web::Form st(root, "status", "Output");
  st.add(web::Switch("on", s.on).label("Output").icon("PowerSettingsNew"));
  st.add(web::Text("hw", ESPRack::hwStateName(s.hwState)).readOnly()
             .label("Hardware").icon("Memory").color(ok ? "success" : "warning"));
  if (s.hwReason[0]) {
    st.add(web::Text("hw_reason", s.hwReason).readOnly().label("Hardware note").icon("Info"));
  }

  web::Form set(root, "settings", "Pins");
  set.add(web::Switch("enabled", s.enabled).label("Enabled").icon("PowerSettingsNew"));
  set.add(web::Number("out_pin", s.outPin).min(-1).max(48).format("0").label("Output pin (-1 = none)").icon("Output"));
  set.add(web::Number("button_pin", s.buttonPin).min(-1).max(48).format("0")
              .label("Button pin (-1 = none)").icon("Input"));
  set.add(web::Switch("active_low", s.activeLow).label("Output active low").icon("SwapVert"));
  set.add(web::Message("m_pins", "A pin that is reserved or used by another module is refused; "
                                 "see System > Hardware.").level("info"));
}

void TemplateExtHwState::readState(TemplateExtHwState& s, JsonObject& root) {
  root["on"]         = s.on;
  root["hw"]         = ESPRack::hwStateName(s.hwState);
  root["hw_reason"]  = (char*)s.hwReason;  // copied
  root["enabled"]    = s.enabled;
  root["out_pin"]    = s.outPin;
  root["button_pin"] = s.buttonPin;
  root["active_low"] = s.activeLow;
}

// ===================== service =====================

TemplateExtHwService::TemplateExtHwService(ESPRack::ExtHwModule& owner, ESPRack::HwRegistry* hw,
                                           ConfigManager* cfgMgr, const TemplateExtHwPins& factory)
    : StatefulService<TemplateExtHwState>(),
      _owner(owner),
      _hw(hw),
      _factory(factory),
      // Plain updater for the file: saved pins apply as they are; a clash
      // shows up as Conflict in the boot pass instead of losing the file.
      // An empty payload (no file yet, reset to defaults) = the ctor pins.
      _cfg(cfgMgr, owner.configId(), owner.configPath(), 1536, this,
           TemplateExtHwState::readConfig,
           [this](JsonObject& root, TemplateExtHwState& s) {
             if (root.isNull() || root.size() == 0) {
               s.outPin = _factory.outPin;
               s.buttonPin = _factory.buttonPin;
               return StateUpdateResult::CHANGED;
             }
             return TemplateExtHwState::update(root, s);
           },
           false, nullptr, TemplateExtHwState::buildForm) {
  snprintf(_wsPath, sizeof(_wsPath), "/ws/%s", owner.instanceId());
  snprintf(_toggleId, sizeof(_toggleId), "%s.toggle", owner.instanceId());
  snprintf(_title, sizeof(_title), "Output %s", owner.instanceId());

  // No file yet = the factory pins (the ctor args).
  updateWithoutPropagation([this](TemplateExtHwState& s) {
    s.outPin = _factory.outPin;
    s.buttonPin = _factory.buttonPin;
    return StateUpdateResult::CHANGED;
  });

  addUpdateHandler([this](const String& origin) {
    _cfg.saveIfChanged(origin);
    if (_hw) _hw->update(_owner);  // new pins: re-claim (right away, not in loop)
    refreshHwState();
    _applyPending = true;          // the pins are switched in loop()
    ESPRack::wakeLoop();
  }, false);
}

// Web writes (UI save, POST /state, WS): apply to a copy, ask the registry
// whether the new pins are free, then commit — or refuse with the reason
// (400, the snackbar shows it) and leave everything as it was.
StateUpdateResult TemplateExtHwService::updateChecked(JsonObject& root, TemplateExtHwState& s) {
  TemplateExtHwState next = s;
  const StateUpdateResult r = TemplateExtHwState::update(root, next);
  if (r != StateUpdateResult::CHANGED) return r;
  if (_hw) {
    HwDescriptor d;
    next.describeHw(d);
    const ESPRack::HwCheck c = _hw->validate(_owner, d);
    if (!c.ok) {
      ESPRack::setUpdateError(c.reason);
      return StateUpdateResult::ERROR;
    }
  }
  s = next;
  return StateUpdateResult::CHANGED;
}

void TemplateExtHwService::registerManifest(WebManager* web) {
  if (!web) return;
  auto checked = [this](JsonObject& root, TemplateExtHwState& s) { return updateChecked(root, s); };

  // Feature id = instance id: /api/v1/<id>/form|state, WS channel <id>.
  _feature = web->registerFeature<TemplateExtHwState>(
      WebFeature(_owner.featureId())
          .title(_title)
          .version("0.1.2")
          .component(WebComponent::DynamicSettings)
          .authenticated()
          .menu(_owner.hwMenu(_title).icon("ToggleOn").order(61))  // placed by the registry
          .ws(_wsPath)
          .tab(WebTab("status", "Status").icon("Monitor").live())
          .tab(WebTab("settings", "Pins").icon("Tune").postable().adminOnly())
          .build(),
      this,
      TemplateExtHwState::buildForm, checked,
      TemplateExtHwState::readState, checked,
      WEBFEATURE_BUFFER_AUTO, 512);

  // An owner drives the output itself: no manual toggle then.
  if (_owner.hwOwned()) return;
  web->registerAction(
      WebAction(_toggleId, "Toggle")
          .icon(WebIcon::Refresh).color("primary")
          .authenticated()
          .success("Toggled")
          .handler([this](AsyncWebServerRequest* r) {
            setOutput(!output());
            r->send(200, "application/json", "{\"ok\":true}");
          }));
}

void TemplateExtHwService::describeHw(HwDescriptor& d) {
  (void)_cfg.ensureLoaded();  // the boot pass runs before begin()
  read([&](TemplateExtHwState& s) { s.describeHw(d); });
}

void TemplateExtHwService::begin() {
  (void)_cfg.ensureLoaded();
  refreshHwState();  // the boot pass has run
  applyPins();
}

void TemplateExtHwService::loop() {
  // Load + store, not exchange (no RMW atomics on a heap object).
  if (_applyPending.load()) {
    _applyPending.store(false);
    applyPins();
  }
  pollButton();
}

uint32_t TemplateExtHwService::loopWaitMs(uint32_t nowMs) const {
  (void)nowMs;
  if (_applyPending.load()) return 0;
  // The button is polled (debounce) at the default cadence; an edge
  // interrupt calling ESPRack::wakeLoopFromISR() would make it an event.
  return _drivenButton >= 0 ? ESPRACK_LOOP_DEFAULT_WAIT_MS : ESPRACK_LOOP_IDLE_WAIT_MS;
}

void TemplateExtHwService::end() {
  // Leave the output in its idle level; the state on flash restores it.
  if (_drivenOut >= 0) {
    bool activeLow = false;
    read([&](TemplateExtHwState& s) { activeLow = s.activeLow; });
    digitalWrite(_drivenOut, activeLow ? HIGH : LOW);
  }
}

bool TemplateExtHwService::restoreFactoryPins() {
  updateWithoutPropagation([this](TemplateExtHwState& s) {
    s.outPin = _factory.outPin;
    s.buttonPin = _factory.buttonPin;
    return StateUpdateResult::CHANGED;
  });
  return _cfg.saveIfChanged("restoreFactoryHw");
}

bool TemplateExtHwService::setOutput(bool on) {
  bool can = false;
  read([&](TemplateExtHwState& s) { can = s.hwState == HwState::Ok; });
  if (!can) return false;
  update([on](TemplateExtHwState& s) {
    if (s.on == on) return StateUpdateResult::UNCHANGED;
    s.on = on;
    return StateUpdateResult::CHANGED;
  }, "api");
  return true;
}

bool TemplateExtHwService::output() {
  bool on = false;
  read([&](TemplateExtHwState& s) { on = s.on; });
  return on;
}

void TemplateExtHwService::refreshHwState() {
  if (!_hw) return;
  const HwState st = _hw->state(_owner);
  char reason[sizeof(TemplateExtHwState::hwReason)];
  _hw->stateReason(_owner, reason, sizeof(reason));
  updateWithoutPropagation([&](TemplateExtHwState& s) {
    s.hwState = st;
    strlcpy(s.hwReason, reason, sizeof(s.hwReason));
    return StateUpdateResult::CHANGED;
  });
}

void TemplateExtHwService::applyPins() {
  TemplateExtHwState s;
  if (!read([&](TemplateExtHwState& cur) { s = cur; })) {  // state lock timed out: the pins stay, the next pass
    _applyPending.store(true);
    return;
  }
  const bool drive = s.hwState == HwState::Ok;
  const int out = drive ? s.outPin : -1;
  const int button = drive ? s.buttonPin : -1;

  // Let go of what we no longer own. The registry keeps those pins claimed
  // ("releasing") until we say released() below, so no other instance can
  // have started driving them yet. Never touch a pin in Conflict.
  if (_drivenOut >= 0 && _drivenOut != out) pinMode(_drivenOut, INPUT);
  if (_drivenButton >= 0 && _drivenButton != button) pinMode(_drivenButton, INPUT);
  if (_hw) _hw->released(_owner);

  if (out >= 0) {
    if (_drivenOut != out) pinMode(out, OUTPUT);
    digitalWrite(out, (s.on != s.activeLow) ? HIGH : LOW);
  }
  if (button >= 0 && _drivenButton != button) {
    pinMode(button, INPUT_PULLUP);
    _lastButton = digitalRead(button);
  }
  // Ok only says the pins are free. What the hardware does is the module's
  // to report: a button that reads low with its pull-up on is
  // probably stuck or shorted.
  if (_hw) {
    const bool stuck = button >= 0 && !digitalRead(button);
    _hw->setRuntimeNote(_owner, stuck ? "button reads low at start (stuck?)" : nullptr, ESPRack::HwNoteLevel::Warn);
  }
  if (out != _drivenOut || button != _drivenButton) {
    log_i("[%s] pins: out=%d button=%d (%s)", _owner.instanceId(), out, button, ESPRack::hwStateName(s.hwState));
  }
  _drivenOut = out;
  _drivenButton = button;
}

void TemplateExtHwService::pollButton() {
  if (_drivenButton < 0) return;
  const bool level = digitalRead(_drivenButton);
  const uint32_t now = millis();
  if (level == _lastButton || now - _buttonChangeMs < 30) return;  // 30 ms debounce
  _lastButton = level;
  _buttonChangeMs = now;
  if (!level) setOutput(!output());  // pressed (pull-up: active low)
}
