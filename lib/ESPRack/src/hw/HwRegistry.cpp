#include "hw/HwRegistry.h"

#include <ESPAsyncWebServer.h>
#include <stdio.h>

#include "AdaptiveJson.h"
#include "App.h"
#include "WebManager.h"
#include "hw/BoardProfile.h"
#include "web/Form.h"

#define HW_FORM_PATH "/api/v1/system/hardware/form"

namespace ESPRack {

namespace {

[[maybe_unused]] const char* stateLabel(HwState s) {  // logs only, at a low log level
  switch (s) {
    case HwState::Ok: return "Ok";
    case HwState::NotConfigured: return "Not configured";
    case HwState::Conflict: return "Conflict";
    case HwState::Disabled: return "Disabled";
  }
  return "?";
}

#if ESPRACK_HW_TAB
// "GPIO17 tx, GPIO18 rx, UART1 port" into buf.
void describeText(const HwDescriptor& d, char* buf, size_t len) {
  size_t n = 0;
  buf[0] = '\0';
  for (uint8_t i = 0; i < d.count && n + 1 < len; ++i) {
    char what[24];
    hwResourceText(d.res[i], what, sizeof(what));
    const int w = snprintf(buf + n, len - n, "%s%s%s%s", i ? ", " : "", what, d.res[i].role ? " " : "",
                           d.res[i].role ? d.res[i].role : "");
    if (w < 0) break;
    n += static_cast<size_t>(w);
  }
}

// Distinct buses/channels of one kind in use: a shared I2C bus held by three
// devices is one bus; every "any channel" claim is one channel.
unsigned usedOf(const HwClaims& claims, HwKind kind) {
  uint64_t fixed = 0;
  unsigned any = 0;
  for (size_t i = 0; i < claims.size(); ++i) {
    const HwResource& r = claims.at(i).res;
    if (r.kind != kind) continue;
    if (r.index == kHwAnyChannel) {
      ++any;
    } else if (r.index >= 0 && r.index < 64) {
      fixed |= 1ULL << r.index;
    }
  }
  unsigned n = any;
  for (; fixed; fixed &= fixed - 1) ++n;
  return n;
}
#endif  // ESPRACK_HW_TAB

}  // namespace

HwRegistry::HwRegistry() : _mx(xSemaphoreCreateMutex()), _claims(currentBoardInfo()) {}

HwRegistry::~HwRegistry() {
  if (_mx) vSemaphoreDelete(_mx);
}

// ---- instances --------------------------------------------------------------

void HwRegistry::addInstance(ExtHwModule* m) {
  if (!m) return;
  m->_hw = this;
  Inst x{};
  x.m = m;
  x.type = m->typeName();
  x.version = "";
  x.state = HwState::NotConfigured;
  _inst.push_back(x);
}

void HwRegistry::addRejected(const ExtHwModule& m, const char* version, const char* why) {
  Rejected r{};
  strlcpy(r.id, m.instanceId(), sizeof(r.id));
  strlcpy(r.type, m.typeName() ? m.typeName() : "?", sizeof(r.type));
  r.version = version ? version : "";
  r.why = why;
  _rejected.push_back(r);
}

ExtHwModule* HwRegistry::findInstance(const char* id) const {
  if (!id) return nullptr;
  for (const Inst& x : _inst) {
    if (strcmp(x.m->instanceId(), id) == 0) return x.m;
  }
  return nullptr;
}

HwRegistry::Inst* HwRegistry::instOf(const ExtHwModule& who) {
  for (Inst& x : _inst) {
    if (x.m == &who) return &x;
  }
  return nullptr;
}

const HwRegistry::Inst* HwRegistry::instOf(const ExtHwModule& who) const {
  for (const Inst& x : _inst) {
    if (x.m == &who) return &x;
  }
  return nullptr;
}

// ---- claims -----------------------------------------------------------------

void HwRegistry::evaluate(Inst& x, const HwDescriptor& d) {
  const char* id = x.m->instanceId();
  x.desc = d;
  x.reason[0] = '\0';
  if (d.disabled) {
    x.state = HwState::Disabled;
    _claims.remove(id);
    return;
  }
  const HwCheck c = _claims.check(id, d);
  if (!c.ok) {
    x.state = HwState::Conflict;
    strlcpy(x.reason, c.reason, sizeof(x.reason));
    _claims.remove(id);
    return;
  }
  for (uint8_t i = 0; i < d.count; ++i) {
    const HwResource& r = d.res[i];
    if (r.index == kHwNotConfigured && !(r.flags & kHwOptional)) {
      x.state = HwState::NotConfigured;
      snprintf(x.reason, sizeof(x.reason), "%s not set", r.role ? r.role : hwKindName(r.kind));
      _claims.remove(id);
      return;
    }
  }
  x.state = HwState::Ok;
  if (c.warning) strlcpy(x.reason, c.reason, sizeof(x.reason));
  _claims.set(id, d);
}

void HwRegistry::boot() {
  // Install order = claim order: the first claimer of a resource wins.
  for (Inst& x : _inst) {
    ModuleDescriptor md;
    x.m->describe(md);
    x.version = md.version ? md.version : "";
    HwDescriptor d;
    x.m->describeHw(d);  // module code: outside the lock
    lock();
    evaluate(x, d);
    const HwState st = x.state;
    char reason[sizeof(x.reason)];
    strlcpy(reason, x.reason, sizeof(reason));
    unlock();
    if (st == HwState::Conflict) {
      log_e("[hw] %s: Conflict — %s (hardware left untouched)", x.m->instanceId(), reason);
    } else if (reason[0]) {
      log_w("[hw] %s: %s — %s", x.m->instanceId(), stateLabel(st), reason);
    } else {
      log_i("[hw] %s: %s", x.m->instanceId(), stateLabel(st));
    }
  }
}

HwCheck HwRegistry::validate(const ExtHwModule& who, const HwDescriptor& proposed) const {
  if (proposed.disabled) return HwCheck{};
  lock();
  HwCheck c = _claims.check(who.instanceId(), proposed);
  unlock();
  return c;
}

HwState HwRegistry::update(ExtHwModule& who) {
  HwDescriptor d;
  who.describeHw(d);  // module code: outside the lock
  lock();
  Inst* x = instOf(who);
  if (!x) {
    unlock();
    return HwState::Ok;  // not registered (installed with install<>()?)
  }
  const HwState before = x->state;
  char reason[sizeof(x->reason)];
  strlcpy(reason, x->reason, sizeof(reason));
  evaluate(*x, d);
  const HwState after = x->state;
  const bool changed = after != before || strcmp(reason, x->reason) != 0;
  strlcpy(reason, x->reason, sizeof(reason));
  unlock();
  if (changed) {
    if (after == HwState::Conflict) {
      log_e("[hw] %s: Conflict — %s", who.instanceId(), reason);
    } else {
      log_i("[hw] %s: %s%s%s", who.instanceId(), stateLabel(after), reason[0] ? " — " : "", reason);
    }
  }
  return after;
}

HwState HwRegistry::state(const ExtHwModule& who) const {
  lock();
  const Inst* x = instOf(who);
  const HwState s = x ? x->state : HwState::Ok;
  unlock();
  return s;
}

void HwRegistry::stateReason(const ExtHwModule& who, char* buf, size_t len) const {
  if (!buf || !len) return;
  lock();
  const Inst* x = instOf(who);
  strlcpy(buf, x ? x->reason : "", len);
  unlock();
}

bool HwRegistry::reserveGpio(int8_t pin, const char* reason) {
  lock();
  BoardInfo& b = _claims.board();
  const bool ok = b.exists(pin) && b.addNote(pin, PinLevel::Reserved, reason ? reason : "reserved");
  unlock();
  if (!ok) log_w("[hw] reserveGpio(%d, %s) ignored: no such GPIO or the table is full", pin, reason ? reason : "");
  return ok;
}

bool HwRegistry::heldByOther(const ExtHwModule& who, HwKind kind, int index) const {
  const bool pin = hwIsPin(kind);
  bool held = false;
  lock();
  for (size_t i = 0; i < _claims.size() && !held; ++i) {
    const HwClaim& c = _claims.at(i);
    if (strcmp(c.owner, who.instanceId()) == 0 || c.res.index != index) continue;
    held = pin ? hwIsPin(c.res.kind) : c.res.kind == kind;
  }
  unlock();
  return held;
}

bool HwRegistry::reserved(int gpio, const char** reason) const {
  lock();
  const PinNote* n = _claims.board().note(gpio);
  const bool r = n && n->level == PinLevel::Reserved;
  if (r && reason) *reason = n->reason;
  unlock();
  return r;
}

// ---- System -> Hardware -----------------------------------------------------

void HwRegistry::registerManifest(WebManager* web) {
  if (!web) return;

#if ESPRACK_HW_TAB
  WebPageSpec page;
  page.id = "systemHardware";
  page.title = "Hardware";
  page.component = "";  // contributes a tab to the compound 'system' feature
  page.auth = WebAuthLevel::Authenticated;
  WebEndpointMeta e;
  e.method = "GET";
  e.path = HW_FORM_PATH;
  e.role = "hardwareForm";
  e.auth = WebAuthLevel::Authenticated;
  e.handler = [this](AsyncWebServerRequest* r) {
    sendAdaptiveJson(r, [this](JsonObject& o) { buildForm(o); }, 1536);
  };
  e.internalHandler = [this](JsonVariant /*in*/, JsonVariant out, int& status) {
    JsonObject o = out.to<JsonObject>();
    buildForm(o);
    status = 200;
  };
  page.endpoints.push_back(std::move(e));
  web->registerPage(std::move(page));

  WebTabSpec tab;
  tab.key = "hardware";
  tab.title = "Hardware";
  tab.restPath = HW_FORM_PATH;
  tab.postable = false;
  tab.auth = WebAuthLevel::Authenticated;
  tab.order = 14;  // after Identity (10) and Resources (12)
  web->addTabToFeature("system", tab);
#endif

  if (_inst.empty()) return;
  WebActionSpec act;
  act.id = "system.restoreFactoryHw";
  act.title = "Restore factory pins";
  act.icon = "SettingsBackupRestore";
  act.color = "warning";
  act.method = "POST";
  act.auth = WebAuthLevel::Admin;
  act.confirm = "Move every external hardware module back to its factory pins and restart?";
  act.successMessage = "Factory pins restored, restarting";
  act.handler = [this](AsyncWebServerRequest* r) {
    _restorePending = true;  // instances are touched on the loop task
    r->send(200, "application/json", "{\"queued\":true}");
  };
  web->registerAction(std::move(act));
}

void HwRegistry::loop() {
  if (!_restorePending.exchange(false)) return;
  int restored = 0;
  for (Inst& x : _inst) {
    if (x.m->restoreFactoryHw()) {
      ++restored;
      log_w("[hw] %s: factory pins restored", x.m->instanceId());
    }
  }
  log_w("[hw] restore factory pins: %d instance(s), restarting", restored);
  restartDevice(kRestartHwRestore);
}

#if ESPRACK_HW_TAB
void HwRegistry::buildForm(JsonObject& root) const {
  web::Form f(root, "hardware", "External hardware");

  lock();
  const BoardInfo& b = _claims.board();
  char line[160];
  snprintf(line, sizeof(line), "%s, %u GPIOs", b.chipName, (unsigned)b.gpioCount);
  f.add(web::Text("chip", line).readOnly().label("Chip").icon("Memory"));

  const struct {
    const char* name;
    HwKind kind;
    uint8_t total;
  } budget[] = {{"UART", HwKind::Uart, b.uarts},    {"I2C", HwKind::I2cBus, b.i2cBuses},
                {"SPI", HwKind::SpiBus, b.spiBuses}, {"RMT TX", HwKind::RmtTx, b.rmtTx},
                {"RMT RX", HwKind::RmtRx, b.rmtRx},  {"LEDC", HwKind::Ledc, b.ledc},
                {"PCNT", HwKind::PcntUnit, b.pcnt}};
  size_t n = 0;
  for (const auto& x : budget) {
    const int w = snprintf(line + n, sizeof(line) - n, "%s%s %u/%u", n ? " · " : "", x.name,
                           usedOf(_claims, x.kind), (unsigned)x.total);
    if (w < 0 || (size_t)w >= sizeof(line) - n) break;
    n += (size_t)w;
  }
  f.add(web::Text("budget", line).readOnly().label("Peripheral claims (used/available)").icon("Tune"));

  f.add(web::Table("instances")
            .col("id", "Instance", "text")
            .col("type", "Type", "text")
            .col("hw", "Hardware", "text")
            .col("state", "State", "text")
            .col("note", "Note", "text")
            .label("Instances"));
  JsonArray rows = f.rowsOf("instances");
  for (const Inst& x : _inst) {
    JsonObject r = rows.createNestedObject();
    r["id"] = x.m->instanceId();
    snprintf(line, sizeof(line), "%s@%s", x.type ? x.type : "?", x.version);
    r["type"] = line;
    describeText(x.desc, line, sizeof(line));
    r["hw"] = line;
    r["state"] = stateLabel(x.state);
    r["note"] = (char*)x.reason;  // copied: the doc outlives the lock
  }
  for (const Rejected& x : _rejected) {
    JsonObject r = rows.createNestedObject();
    r["id"] = (char*)x.id;
    snprintf(line, sizeof(line), "%s@%s", x.type, x.version);
    r["type"] = line;
    r["hw"] = "";
    r["state"] = "Not installed";
    r["note"] = x.why;
  }

  f.add(web::Table("pins")
            .col("gpio", "GPIO", "number", "0")
            .col("level", "Use", "text")
            .col("reason", "Why", "text")
            .label("Reserved and caution pins"));
  JsonArray pins = f.rowsOf("pins");
  for (uint8_t i = 0; i < b.noteCount; ++i) {
    JsonObject r = pins.createNestedObject();
    r["gpio"] = b.notes[i].gpio;
    r["level"] = b.notes[i].level == PinLevel::Reserved ? "reserved" : "caution";
    r["reason"] = b.notes[i].reason;
  }
  unlock();

  if (!_inst.empty()) f.add(web::Action("a_restore").ref("system.restoreFactoryHw"));
}
#endif  // ESPRACK_HW_TAB

}  // namespace ESPRack
