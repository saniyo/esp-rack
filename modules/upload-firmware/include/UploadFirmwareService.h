#ifndef UploadFirmwareService_h
#define UploadFirmwareService_h

#include <Arduino.h>

#ifdef ESP32
#include <WiFi.h>
#include <AsyncTCP.h>
#include <net/UploadPump.h>
#include <system/OtaWriter.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#endif

#include <ESPAsyncWebServer.h>
#include <SecurityManager.h>
#include <App.h>  // ESPRack::restartDevice (core) — not the restart module

#define UPLOAD_FIRMWARE_PATH      "/api/v1/system/upload/firmware"
#define UPLOAD_FIRMWARE_FORM_PATH "/api/v1/system/upload/form"

class WebManager;

// The upload page's firmware POST. ESP32: the image goes into the next OTA
// slot through ESPRack::OtaWriter, driven by an ESPRack::UploadPump: one
// 4 KB sector per write gate slot (power/WriteGate.h), never a wait on
// async_tcp. With a free gate (no module holds windows) every chunk is
// written as it comes, as before. The answer comes once the image is
// written and checked (200, else 500 with the reason); the device restarts
// when the client has gone after a 200. A client that leaves before the
// last byte aborts the image (nothing is set to boot); one that leaves
// while it is checked and set to boot: that completes, no restart. One
// image at a time: 503 while auto-update, ArduinoOTA or another upload
// holds the update (OtaWriter::reserve at the first chunk).
class UploadFirmwareService {
 public:
  UploadFirmwareService(AsyncWebServer* server, SecurityManager* securityManager);

  // Contribute the 'upload' tab to the compound 'system' feature.
  void registerManifest(WebManager* web);

  // Emits the 'upload' sub-form: warning message + drag-drop upload widget
  // whose url= points at the legacy UPLOAD_FIRMWARE_PATH handler.
  static void buildForm(JsonObject& root);

 private:
  SecurityManager* _securityManager;
#ifdef ESP32
  ESPRack::OtaWriter _ota;    // reserved at the first chunk; then its sink is called by the pump only
  ESPRack::UploadPump _pump;  // one upload at a time (503 for another)
#else
  // The request whose authorised upload ended with a valid image. Only that
  // one restarts the device (one firmware update at a time: Update is global).
  AsyncWebServerRequest* _finished = nullptr;
  static void handleEarlyDisconnect();
#endif
  void handleUpload(AsyncWebServerRequest* request,
                    const String& filename,
                    size_t index,
                    uint8_t* data,
                    size_t len,
                    bool final);
  void uploadComplete(AsyncWebServerRequest* request);
  // Refused before the pump took the request (auth, size, busy): answered
  // here, the rest of the body is ignored.
  void handleError(AsyncWebServerRequest* request, int code);

  void uploadForm(AsyncWebServerRequest* request);
};

#endif  // end UploadFirmwareService_h
