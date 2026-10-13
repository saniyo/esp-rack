#include "hw/ExtHwModule.h"

#include "hw/HwRegistry.h"

namespace ESPRack {

bool isValidInstanceId(const char* id) {
  if (!id || !*id) return false;
  size_t n = 0;
  for (const char* p = id; *p; ++p, ++n) {
    const char c = *p;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok || n >= ExtHwModule::kIdMax) return false;
  }
  return strcmp(id, "system") != 0;
}

ExtHwModule::ExtHwModule(const char* instanceId) : _idValid(isValidInstanceId(instanceId)) {
  strlcpy(_id, instanceId ? instanceId : "", sizeof(_id));
  snprintf(_cfgId, sizeof(_cfgId), "hw-%s", _id);
  snprintf(_path, sizeof(_path), "/config/hw-%s.json", _id);
}

HwState ExtHwModule::hwState() const { return _hw ? _hw->state(*this) : HwState::Ok; }

void ExtHwModule::hwReleased() {
  if (_hw) _hw->released(*this);
}

void ExtHwModule::hwNote(const char* note, HwNoteLevel level) {
  if (_hw) _hw->setRuntimeNote(*this, note, level);
}

const char* ExtHwModule::actionId(const char* verb, char* buf, size_t len) const {
  if (!buf || !len) return "";
  snprintf(buf, len, "%s.%s", _id, verb ? verb : "");
  return buf;
}

}  // namespace ESPRack
