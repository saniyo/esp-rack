#include <FeaturesService.h>

FeaturesService::FeaturesService(AsyncWebServer* server) {
  server->on(FEATURES_SERVICE_PATH, HTTP_GET, std::bind(&FeaturesService::features, this, std::placeholders::_1));
}

void FeaturesService::features(AsyncWebServerRequest* request) {
  AsyncJsonResponse* response = new AsyncJsonResponse(false, MAX_FEATURES_SIZE);
  JsonObject root = response->getRoot();
#if FT_ENABLED(FT_PROJECT)
  root["project"] = true;
#else
  root["project"] = false;
#endif
#if FT_ENABLED(FT_SECURITY)
  root["security"] = true;
#else
  root["security"] = false;
#endif
#if FT_ENABLED(FT_TELEGRAM)
  root["telegram"] = true;
#else
  root["telegram"] = false;
#endif
#if FT_ENABLED(FT_MQTT)
  root["mqtt"] = true;
#else
  root["mqtt"] = false;
#endif
#if FT_ENABLED(FT_NTP)
  root["ntp"] = true;
#else
  root["ntp"] = false;
#endif
#if FT_ENABLED(FT_OTA)
  root["ota"] = true;
#else
  root["ota"] = false;
#endif
#if FT_ENABLED(FT_UPLOAD_FIRMWARE)
  root["upload_firmware"] = true;
#else
  root["upload_firmware"] = false;
#endif
#if FT_ENABLED(FT_AUTO_UPDATE)
  root["auto_update"] = true;
#else
  root["auto_update"] = false;
#endif
#if FT_ENABLED(FT_SYSTEM_INFO)
  root["system_info"] = true;
#else
  root["system_info"] = false;
#endif
  response->setLength();
  request->send(response);
}
