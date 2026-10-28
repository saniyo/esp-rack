#ifndef ArduinoJsonJWT_H
#define ArduinoJsonJWT_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <libb64/cdecode.h>
#include <libb64/cencode.h>

#ifdef ESP32
#include <mbedtls/md.h>
#elif defined(ESP8266)
#include <bearssl/bearssl_hmac.h>
#endif

class ArduinoJsonJWT {
 private:
  String _secret;

  const String JWT_HEADER = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
  const int JWT_HEADER_SIZE = JWT_HEADER.length();

  String sign(String& value);

  static String encode(const char* cstr, int len);
  static String decode(String value);

 public:
  ArduinoJsonJWT(String secret);

  void setSecret(String secret);
  String getSecret();

  String buildJWT(JsonObject& payload);
  // Invalid: a wrong or damaged token. NoMemory: it could not be checked
  // (an allocation failed): not the token's fault, the caller says so.
  enum class Result : uint8_t { Ok, Invalid, NoMemory };
  Result parseJWT(String jwt, JsonDocument& jsonDocument);
};

#endif
