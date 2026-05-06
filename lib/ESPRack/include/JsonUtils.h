#ifndef JsonUtils_h
#define JsonUtils_h

#include <Arduino.h>
#include <IPUtils.h>
#include <ArduinoJson.h>

class JsonUtils {
 public:
  // Arduino-ESP32 3.x added an `IPAddress(const char*)` ctor which made
  // the historical String-vs-IPAddress overload pair ambiguous when
  // callers pass a string literal (e.g. FACTORY_AP_LOCAL_IP). The String
  // overload now takes a const char* directly so literals bind cleanly,
  // and the IPAddress overload stays for callers that already have one.
  static void readIP(JsonObject& root, const String& key, IPAddress& ip, const char* def) {
    IPAddress defaultIp = {};
    if (!def || !defaultIp.fromString(def)) {
      defaultIp = INADDR_NONE;
    }
    readIP(root, key, ip, defaultIp);
  }
  static void readIP(JsonObject& root, const String& key, IPAddress& ip, const IPAddress& defaultIp = INADDR_NONE) {
    if (!root[key].is<String>() || !ip.fromString(root[key].as<String>())) {
      ip = defaultIp;
    }
  }
  static void writeIP(JsonObject& root, const String& key, const IPAddress& ip) {
    if (IPUtils::isSet(ip)) {
      root[key] = ip.toString();
    }
  }
};

#endif  // end JsonUtils
