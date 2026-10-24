#include "hw/HwClaims.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "hw/BoardTables.h"

// On the target only the running chip's table is compiled (the others are
// dead weight); host tests compile them all.
#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#define HW_TAB_ESP32 CONFIG_IDF_TARGET_ESP32
#define HW_TAB_S3 CONFIG_IDF_TARGET_ESP32S3
#define HW_TAB_C3 CONFIG_IDF_TARGET_ESP32C3
#define HW_TAB_C6 CONFIG_IDF_TARGET_ESP32C6
#else
#define HW_TAB_ESP32 1
#define HW_TAB_S3 1
#define HW_TAB_C3 1
#define HW_TAB_C6 1
#endif

namespace ESPRack {

namespace {

bool sameOwner(const char* a, const char* b) { return a == b || (a && b && strcmp(a, b) == 0); }

bool isPinKind(HwKind k) { return hwIsPin(k); }
bool isRmt(HwKind k) { return k == HwKind::RmtTx || k == HwKind::RmtRx; }
int busOf(const HwResource& r) { return r.bus < 0 ? 0 : r.bus; }
bool sameRole(const HwResource& a, const HwResource& b) {
  return a.role == b.role || (a.role && b.role && strcmp(a.role, b.role) == 0);
}
// Kinds whose index is a peripheral instance or channel number.
bool isChannelKind(HwKind k) {
  return k == HwKind::RmtTx || k == HwKind::RmtRx || k == HwKind::Ledc || k == HwKind::PcntUnit;
}

const char* roleOf(const HwResource& r) { return r.role && *r.role ? r.role : hwKindName(r.kind); }

template <typename... A>
void fail(HwCheck& c, const char* fmt, A... a) {
  if (!c.ok) return;  // keep the first error
  c.ok = false;
  c.warning = false;
  snprintf(c.reason, sizeof(c.reason), fmt, a...);
}

template <typename... A>
void warn(HwCheck& c, const char* fmt, A... a) {
  if (!c.ok || c.warning) return;  // errors and the first warning win
  c.warning = true;
  snprintf(c.reason, sizeof(c.reason), fmt, a...);
}

uint8_t budgetOf(const BoardInfo& b, HwKind k) {
  switch (k) {
    case HwKind::RmtTx: return b.rmtTx;
    case HwKind::RmtRx: return b.rmtRx;
    case HwKind::Ledc: return b.ledc;
    case HwKind::PcntUnit: return b.pcnt;
    case HwKind::Uart: return b.uarts;
    case HwKind::I2cBus: return b.i2cBuses;
    case HwKind::SpiBus: return b.spiBuses;
    default: return 0;
  }
}

const char* channelName(HwKind k) {
  switch (k) {
    case HwKind::RmtTx: return "RMT TX channels";
    case HwKind::RmtRx: return "RMT RX channels";
    case HwKind::Ledc: return "LEDC channels";
    case HwKind::PcntUnit: return "PCNT units";
    default: return "channels";
  }
}

// Do two resources denote the same hardware? sharedRmt: TX and RX channel n
// are one channel (classic ESP32).
bool sameThing(const HwResource& a, const HwResource& b, bool sharedRmt) {
  if (isPinKind(a.kind) && isPinKind(b.kind)) return a.index == b.index;
  if (sharedRmt && isRmt(a.kind) && isRmt(b.kind)) return a.index == b.index;
  if (a.kind != b.kind) return false;
  if (a.kind == HwKind::I2cAddr) return busOf(a) == busOf(b) && a.index == b.index;
  if (a.kind == HwKind::Other) return false;
  return a.index == b.index;
}

// Both shared; a pin only as the same pin of the same bus.
bool sharable(const HwResource& a, const HwResource& b) {
  if (!(a.flags & kHwShared) || !(b.flags & kHwShared) || a.kind == HwKind::I2cAddr) return false;
  if (isPinKind(a.kind)) return a.kind == b.kind && a.bus == b.bus && sameRole(a, b);
  return true;
}

}  // namespace

const char* hwKindName(HwKind k) {
  switch (k) {
    case HwKind::Gpio: return "gpio";
    case HwKind::Uart: return "uart";
    case HwKind::I2cBus: return "i2c";
    case HwKind::I2cAddr: return "i2c-addr";
    case HwKind::SpiBus: return "spi";
    case HwKind::SpiCs: return "spi-cs";
    case HwKind::RmtTx: return "rmt-tx";
    case HwKind::RmtRx: return "rmt-rx";
    case HwKind::Adc: return "adc";
    case HwKind::Ledc: return "ledc";
    case HwKind::PcntUnit: return "pcnt";
    case HwKind::Other: return "other";
  }
  return "?";
}

const char* hwStateName(HwState s) {
  switch (s) {
    case HwState::Ok: return "Ok";
    case HwState::NotConfigured: return "NotConfigured";
    case HwState::Conflict: return "Conflict";
    case HwState::Disabled: return "Disabled";
  }
  return "?";
}

const char* hwResourceText(const HwResource& r, char* buf, size_t len) {
  if (!buf || !len) return "";
  if (r.index == kHwNotConfigured) {
    snprintf(buf, len, "%s -", hwKindName(r.kind));
    return buf;
  }
  switch (r.kind) {
    case HwKind::Gpio:
    case HwKind::Adc:
    case HwKind::SpiCs: snprintf(buf, len, "GPIO%d", r.index); break;
    case HwKind::Uart: snprintf(buf, len, "UART%d", r.index); break;
    case HwKind::I2cBus: snprintf(buf, len, "I2C%d", r.index); break;
    case HwKind::I2cAddr: snprintf(buf, len, "I2C%d 0x%02x", r.bus < 0 ? 0 : r.bus, r.index); break;
    case HwKind::SpiBus: snprintf(buf, len, "SPI%d", r.index); break;
    default:
      if (r.index == kHwAnyChannel) {
        snprintf(buf, len, "%s any", hwKindName(r.kind));
      } else {
        snprintf(buf, len, "%s %d", hwKindName(r.kind), r.index);
      }
  }
  return buf;
}

bool BoardInfo::addNote(int8_t gpio, PinLevel level, const char* reason) {
  if (noteCount >= kMaxNotes) return false;
  notes[noteCount++] = PinNote{gpio, level, reason};
  return true;
}

const PinNote* BoardInfo::note(int gpio) const {
  const PinNote* found = nullptr;
  for (uint8_t i = 0; i < noteCount; ++i) {
    if (notes[i].gpio != gpio) continue;
    if (notes[i].level == PinLevel::Reserved) return &notes[i];
    if (!found) found = &notes[i];
  }
  return found;
}

BoardInfo boardInfoFor(HwChip chip, const BoardFlags& f) {
  using namespace hwtab;
  BoardInfo b;
  b.chip = chip;
  b.uart0Console = f.uart0Console;
  auto reserve = [&b](const int8_t* pins, size_t n, const char* why) {
    for (size_t i = 0; i < n; ++i) b.addNote(pins[i], PinLevel::Reserved, why);
  };
  auto budget = [&b](const ChipBudget& c) {
    b.uarts = c.uarts; b.i2cBuses = c.i2cBuses; b.spiBuses = c.spiBuses;
    b.rmtTx = c.rmtTx; b.rmtRx = c.rmtRx; b.rmtTotal = c.rmtTotal; b.ledc = c.ledc; b.pcnt = c.pcnt;
  };
  // UART0 not the console: free, but the ROM still prints its boot log on TX.
  auto uart0 = [&b, &f, &reserve](const int8_t* pins, size_t n) {
    if (f.uart0Console) {
      reserve(pins, n, "UART0 console");
    } else {
      for (size_t i = 0; i < n; ++i) b.addNote(pins[i], PinLevel::Caution, "UART0: ROM boot log at reset");
    }
  };
  (void)reserve;  // unused on a target without a pin table
  (void)budget;
  (void)uart0;
  switch (chip) {
#if HW_TAB_ESP32
    case HwChip::Esp32:
      b.chipName = "ESP32";
      b.gpioCount = 40;
      b.validMask = kEsp32ValidMask;
      b.outputMask = kEsp32OutputMask;
      b.adc2Mask = maskOf(kEsp32Adc2);
      budget(kEsp32Budget);
      reserve(kEsp32Flash, sizeof(kEsp32Flash), "SPI flash");
      if (f.psram) reserve(kEsp32Psram, sizeof(kEsp32Psram), "PSRAM");
      uart0(kEsp32Uart0, sizeof(kEsp32Uart0));
      b.addNote(0, PinLevel::Caution, "strapping: boot mode (low at reset = download)");
      b.addNote(2, PinLevel::Caution, "strapping: boot mode");
      b.addNote(5, PinLevel::Caution, "strapping: SDIO timing");
      b.addNote(12, PinLevel::Caution, "strapping MTDI: high at reset selects 1.8 V flash — 3.3 V flash won't boot");
      b.addNote(15, PinLevel::Caution, "strapping MTDO: boot log / SDIO timing");
      break;
#endif
#if HW_TAB_S3
    case HwChip::Esp32S3:
      b.chipName = "ESP32-S3";
      b.gpioCount = 49;
      b.validMask = b.outputMask = kS3ValidMask;
      b.adc2Mask = maskOf(kS3Adc2);
      budget(kS3Budget);
      reserve(kS3Flash, sizeof(kS3Flash), "SPI flash");
      if (f.psram) b.addNote(kS3PsramCs, PinLevel::Reserved, "PSRAM CS");
      if (f.octalPsramOrFlash) reserve(kS3Octal, sizeof(kS3Octal), "octal PSRAM/flash");
      uart0(kS3Uart0, sizeof(kS3Uart0));
      if (f.usbInUse) reserve(kS3Usb, sizeof(kS3Usb), "USB (console/JTAG)");
      b.addNote(0, PinLevel::Caution, "strapping: boot mode (low at reset = download)");
      b.addNote(3, PinLevel::Caution, "strapping: JTAG source");
      b.addNote(45, PinLevel::Caution, "strapping VDD_SPI: high at reset selects 1.8 V VDD_SPI — bricks N16R8 flash/PSRAM");
      b.addNote(46, PinLevel::Caution, "strapping: boot mode / ROM log");
      break;
#endif
#if HW_TAB_C3
    case HwChip::Esp32C3:
      b.chipName = "ESP32-C3";
      b.gpioCount = 22;
      b.validMask = b.outputMask = kC3ValidMask;
      b.adc2Mask = maskOf(kC3Adc2);
      budget(kC3Budget);
      reserve(kC3Flash, sizeof(kC3Flash), "SPI flash");
      uart0(kC3Uart0, sizeof(kC3Uart0));
      if (f.usbInUse) reserve(kC3Usb, sizeof(kC3Usb), "USB (console/JTAG)");
      b.addNote(2, PinLevel::Caution, "strapping: boot mode");
      b.addNote(8, PinLevel::Caution, "strapping: boot mode / ROM log");
      b.addNote(9, PinLevel::Caution, "strapping: boot mode (low at reset = download)");
      break;
#endif
#if HW_TAB_C6
    case HwChip::Esp32C6:
      b.chipName = "ESP32-C6";
      b.gpioCount = 31;
      b.validMask = b.outputMask = kC6ValidMask;
      b.adc2Mask = 0;  // ADC1 only
      budget(kC6Budget);
      reserve(kC6Flash, sizeof(kC6Flash), "SPI flash");
      uart0(kC6Uart0, sizeof(kC6Uart0));
      if (f.usbInUse) reserve(kC6Usb, sizeof(kC6Usb), "USB (console/JTAG)");
      b.addNote(4, PinLevel::Caution, "strapping MTMS: SDIO timing");
      b.addNote(5, PinLevel::Caution, "strapping MTDI: SDIO timing");
      b.addNote(8, PinLevel::Caution, "strapping: boot mode / ROM log");
      b.addNote(9, PinLevel::Caution, "strapping: boot mode (low at reset = download)");
      b.addNote(15, PinLevel::Caution, "strapping: JTAG source");
      break;
#endif
    default:  // Unknown, or a chip whose table isn't compiled in
      // No pin table: only conflicts between instances are checked.
      b.chipName = "unknown chip";
      b.gpioCount = 64;
      b.validMask = b.outputMask = ~0ULL;
      b.uarts = 3; b.i2cBuses = 2; b.spiBuses = 2;
      b.rmtTx = 8; b.rmtRx = 8; b.rmtTotal = 16; b.ledc = 16; b.pcnt = 8;
      break;
  }
  return b;
}

uint8_t HwClaims::countOthers(HwKind kind, const char* owner) const {
  uint8_t n = 0;
  for (const HwClaim& c : _claims) {
    if (c.res.kind == kind && !sameOwner(c.owner, owner) && c.res.index != kHwNotConfigured) ++n;
  }
  for (const HwClaim& c : _reserved) {
    if (c.res.kind == kind) ++n;  // a reserved channel is used by someone
  }
  return n;
}

HwCheck HwClaims::check(const char* owner, const HwDescriptor& d) const {
  HwCheck c;
  const BoardInfo& b = _b;
  const bool sharedRmt = b.rmtTotal < b.rmtTx + b.rmtRx;
  uint8_t mine[static_cast<int>(HwKind::Other) + 1] = {0};

  for (uint8_t i = 0; i < d.count && c.ok; ++i) {
    const HwResource& r = d.res[i];
    if (r.index == kHwNotConfigured) continue;
    char what[24];
    hwResourceText(r, what, sizeof(what));

    // --- what the board allows
    if (isPinKind(r.kind)) {
      if (!b.exists(r.index)) {
        fail(c, "GPIO%d doesn't exist on %s", r.index, b.chipName);
        break;
      }
      if (const PinNote* n = b.note(r.index)) {
        if (n->level == PinLevel::Reserved) {
          fail(c, "GPIO%d reserved: %s", r.index, n->reason);
          break;
        }
        warn(c, "GPIO%d %s", r.index, n->reason);
      }
      if ((r.flags & kHwOutput) && !b.canOutput(r.index)) {
        fail(c, "GPIO%d is input-only on %s", r.index, b.chipName);
        break;
      }
      if (r.kind == HwKind::Adc && b.isAdc2(r.index)) {
        warn(c, "GPIO%d is ADC2: unreliable while Wi-Fi is active", r.index);
      }
      if (r.kind == HwKind::SpiCs && r.bus >= b.spiBuses) {
        fail(c, "SPI%d doesn't exist on %s", r.bus, b.chipName);
        break;
      }
    } else if (r.kind == HwKind::Uart || r.kind == HwKind::I2cBus || r.kind == HwKind::SpiBus) {
      if (r.index < 0 || r.index >= budgetOf(b, r.kind)) {
        fail(c, "%s doesn't exist on %s", what, b.chipName);
        break;
      }
      if (r.kind == HwKind::Uart && r.index == 0 && b.uart0Console) {
        fail(c, "UART0 reserved: console");
        break;
      }
    } else if (r.kind == HwKind::I2cAddr) {
      if (r.index < 0x08 || r.index > 0x77) {
        fail(c, "I2C address 0x%02x is reserved", r.index);
        break;
      }
      if (busOf(r) >= b.i2cBuses) {
        fail(c, "I2C%d doesn't exist on %s", busOf(r), b.chipName);
        break;
      }
    } else if (isChannelKind(r.kind)) {
      const uint8_t budget = budgetOf(b, r.kind);
      if (budget == 0) {
        fail(c, "no %s on %s", channelName(r.kind), b.chipName);
        break;
      }
      if (r.index != kHwAnyChannel && (r.index < 0 || r.index >= budget)) {
        fail(c, "%s doesn't exist on %s", what, b.chipName);
        break;
      }
      const uint8_t k = static_cast<uint8_t>(r.kind);
      ++mine[k];
      if (countOthers(r.kind, owner) + mine[k] > budget) {
        fail(c, "%s exhausted (%u on %s)", channelName(r.kind), (unsigned)budget, b.chipName);
        break;
      }
      // classic ESP32: TX and RX come from one pool of rmtTotal channels
      if ((r.kind == HwKind::RmtTx || r.kind == HwKind::RmtRx) && b.rmtTotal < b.rmtTx + b.rmtRx) {
        const uint8_t all = countOthers(HwKind::RmtTx, owner) + countOthers(HwKind::RmtRx, owner) +
                            mine[static_cast<uint8_t>(HwKind::RmtTx)] + mine[static_cast<uint8_t>(HwKind::RmtRx)];
        if (all > b.rmtTotal) {
          fail(c, "RMT channels exhausted (%u on %s)", (unsigned)b.rmtTotal, b.chipName);
          break;
        }
      }
    }

    // --- the same hardware twice inside this instance
    for (uint8_t j = 0; j < i && c.ok; ++j) {
      const HwResource& o = d.res[j];
      if (o.index == kHwNotConfigured || o.index == kHwAnyChannel || r.index == kHwAnyChannel) continue;
      if (sameThing(r, o, sharedRmt) && !sharable(r, o)) fail(c, "%s used twice (%s, %s)", what, roleOf(o), roleOf(r));
    }

    // --- reserved for the consumer / framework
    if (r.index == kHwAnyChannel) continue;
    for (const HwClaim& o : _reserved) {
      if (sameThing(r, o.res, sharedRmt)) {
        fail(c, "%s reserved: %s", what, o.owner ? o.owner : "?");
        break;
      }
    }
    if (!c.ok) break;

    // --- other owners (a releasing claim still holds the hardware)
    const bool busPin = isPinKind(r.kind) && (r.flags & kHwShared) && r.bus >= 0;
    for (const HwClaim& o : _claims) {
      if (sameOwner(o.owner, owner) || o.res.index == kHwNotConfigured || o.res.index == kHwAnyChannel) continue;
      if (sameThing(r, o.res, sharedRmt) && !sharable(r, o.res)) {
        fail(c, o.releasing ? "%s being released by %s (%s)" : "%s used by %s (%s)", what,
             o.owner ? o.owner : "?", roleOf(o.res));
        break;
      }
      // A shared bus has one set of pins: SDA of bus 0 can't be GPIO8 for
      // one device and GPIO4 for the next (the second Wire.begin moves it).
      const HwResource& x = o.res;
      if (busPin && isPinKind(x.kind) && (x.flags & kHwShared) && x.bus == r.bus && sameRole(x, r) &&
          x.index != r.index) {
        fail(c, "%s of bus %d is GPIO%d (%s)", roleOf(r), r.bus, x.index, o.owner ? o.owner : "?");
        break;
      }
    }
  }
  return c;
}

void HwClaims::set(const char* owner, const HwDescriptor& d) {
  remove(owner);
  for (uint8_t i = 0; i < d.count; ++i) {
    if (d.res[i].index == kHwNotConfigured) continue;
    _claims.push_back(HwClaim{owner, d.res[i]});
  }
}

void HwClaims::remove(const char* owner) {
  for (size_t i = _claims.size(); i-- > 0;) {
    if (sameOwner(_claims[i].owner, owner)) _claims.erase(_claims.begin() + static_cast<ptrdiff_t>(i));
  }
}

namespace {
bool sameResource(const HwResource& a, const HwResource& b) {
  return a.kind == b.kind && a.index == b.index && a.bus == b.bus;
}
}  // namespace

void HwClaims::replace(const char* owner, const HwDescriptor& d, bool applied, uint32_t nowMs) {
  // The owner's active claims: kept ones are re-added from d below, dropped
  // ones become releasing (or just go, if nothing was set up under them).
  for (size_t i = _claims.size(); i-- > 0;) {
    HwClaim& c = _claims[i];
    if (!sameOwner(c.owner, owner) || c.releasing) continue;
    bool kept = false;
    for (uint8_t j = 0; j < d.count && !kept; ++j) kept = sameResource(c.res, d.res[j]);
    if (kept || !applied) {
      _claims.erase(_claims.begin() + static_cast<ptrdiff_t>(i));
    } else {
      c.releasing = true;
      c.releasingSince = nowMs;
    }
  }
  for (uint8_t j = 0; j < d.count; ++j) {
    if (d.res[j].index == kHwNotConfigured) continue;
    // Taken back before it was released: it's simply active again.
    for (size_t i = _claims.size(); i-- > 0;) {
      const HwClaim& c = _claims[i];
      if (c.releasing && sameOwner(c.owner, owner) && sameResource(c.res, d.res[j])) {
        _claims.erase(_claims.begin() + static_cast<ptrdiff_t>(i));
      }
    }
    _claims.push_back(HwClaim{owner, d.res[j], false, 0});
  }
}

void HwClaims::releaseAll(const char* owner, bool applied, uint32_t nowMs) {
  for (size_t i = _claims.size(); i-- > 0;) {
    HwClaim& c = _claims[i];
    if (!sameOwner(c.owner, owner) || c.releasing) continue;
    if (!applied) {
      _claims.erase(_claims.begin() + static_cast<ptrdiff_t>(i));
    } else {
      c.releasing = true;
      c.releasingSince = nowMs;
    }
  }
}

void HwClaims::released(const char* owner) {
  for (size_t i = _claims.size(); i-- > 0;) {
    if (_claims[i].releasing && sameOwner(_claims[i].owner, owner)) {
      _claims.erase(_claims.begin() + static_cast<ptrdiff_t>(i));
    }
  }
}

size_t HwClaims::releasingCount(const char* owner) const {
  size_t n = 0;
  for (const HwClaim& c : _claims) {
    if (c.releasing && sameOwner(c.owner, owner)) ++n;
  }
  return n;
}

uint32_t HwClaims::releasingAgeMs(const char* owner, uint32_t nowMs) const {
  uint32_t age = 0;
  for (const HwClaim& c : _claims) {
    if (!c.releasing || !sameOwner(c.owner, owner)) continue;
    const uint32_t a = nowMs - c.releasingSince;  // wraps correctly
    if (a > age) age = a;
  }
  return age;
}

const char* hwStateLabel(HwState s) {
  switch (s) {
    case HwState::Ok: return "Ok";
    case HwState::NotConfigured: return "Not configured";
    case HwState::Conflict: return "Conflict";
    case HwState::Disabled: return "Disabled";
  }
  return "?";
}

void hwUtf8Trim(char* s) {
  if (!s) return;
  const size_t n = strlen(s);
  size_t i = n;
  while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) --i;  // continuation bytes
  if (i == 0) return;
  const unsigned char lead = static_cast<unsigned char>(s[i - 1]);
  const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  if (n - (i - 1) < need) s[i - 1] = '\0';
}

namespace {
// Appends `part` to buf, " · "-separated; stops quietly when full.
void appendPart(char* buf, size_t len, const char* part) {
  if (!part || !*part) return;
  const size_t n = strlen(buf);
  if (n + 1 >= len) return;
  snprintf(buf + n, len - n, "%s%s", n ? " · " : "", part);
  hwUtf8Trim(buf);
}
}  // namespace

void hwStatusText(const HwInstStatus& s, char* state, size_t stateLen, char* note, size_t noteLen) {
  const bool hasNote = s.note && *s.note;
  const bool overdue = s.releasing && s.releasingAgeMs > kHwReleasingWarnMs;
  const char* level = hasNote && s.noteLevel == HwNoteLevel::Fault           ? " · fault"
                      : (hasNote && s.noteLevel == HwNoteLevel::Warn) || overdue ? " · warn"
                                                                             : "";
  if (state && stateLen) snprintf(state, stateLen, "%s%s", hwStateLabel(s.state), level);
  if (!note || !noteLen) return;
  note[0] = '\0';
  appendPart(note, noteLen, s.reason);
  appendPart(note, noteLen, s.note);
  if (s.releasing) {
    char rel[48];
    if (overdue) {
      snprintf(rel, sizeof(rel), "releasing %u for %u s, no released()", (unsigned)s.releasing,
               (unsigned)(s.releasingAgeMs / 1000));
    } else {
      snprintf(rel, sizeof(rel), "releasing %u", (unsigned)s.releasing);
    }
    appendPart(note, noteLen, rel);
  }
}

bool HwClaims::reserve(const HwResource& r, const char* reason) {
  if (isPinKind(r.kind) || r.index < 0) return false;
  _reserved.push_back(HwClaim{reason ? reason : "reserved", r, false, 0});
  return true;
}

}  // namespace ESPRack
