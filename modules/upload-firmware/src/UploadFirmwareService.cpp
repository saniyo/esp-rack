#include <UploadFirmwareService.h>

#include <AdaptiveJson.h>
#include <FormBuilder.h>
#include <WebManager.h>
#include <system/FirmwareUpdate.h>

UploadFirmwareService::UploadFirmwareService(AsyncWebServer* server, SecurityManager* securityManager) :
    _securityManager(securityManager) {
  // Multipart firmware upload — sticks with raw server->on because
  // the 4-arg multipart shape doesn't fit WebEndpointMeta's
  // handler/jsonHandler/internalHandler trio. registerManifest below
  // surfaces the path in the manifest for client-side URL discovery.
  server->on(UPLOAD_FIRMWARE_PATH,
             HTTP_POST,
             std::bind(&UploadFirmwareService::uploadComplete, this, std::placeholders::_1),
             std::bind(&UploadFirmwareService::handleUpload,
                       this,
                       std::placeholders::_1,
                       std::placeholders::_2,
                       std::placeholders::_3,
                       std::placeholders::_4,
                       std::placeholders::_5,
                       std::placeholders::_6));

  // Form-schema GET endpoint was previously bound here via raw
  // server->on; now lives in registerManifest below as a normal
  // WebEndpointMeta entry under the "uploadFirmware" page so it
  // goes through WebManager like every other tab schema.
#ifdef ESP32
  // The pump's FlashWriter job (at setup, on the loop task).
  _pump.attach("fw-upload");
#elif defined(ESP8266)
  Update.runAsync(true);
#endif
}

void UploadFirmwareService::registerManifest(WebManager* web) {
  if (!web) return;
  using namespace std::placeholders;

  // Form-schema GET — was raw server->on in the ctor, now declared
  // through WebManager so the route binding lives next to the
  // manifest metadata (same place an operator/developer looks for
  // "what does this module expose").
  WebPageSpec page;
  page.id        = "uploadFirmware";
  page.title     = "Upload Firmware";
  page.component = "";  // no top-level page route; lives under system tab
  page.auth      = WebAuthLevel::Admin;
  // No menu entry — the system feature owns the visible tab.

  {
    WebEndpointMeta e;
    e.method = "GET";
    e.path   = UPLOAD_FIRMWARE_FORM_PATH;
    e.role   = "form";
    e.auth   = WebAuthLevel::Admin;
    e.handler = std::bind(&UploadFirmwareService::uploadForm, this, _1);
    page.endpoints.push_back(std::move(e));
  }
  {
    // Multipart upload — manifest-visible, binding stays in ctor.
    WebEndpointMeta e;
    e.method = "POST";
    e.path   = UPLOAD_FIRMWARE_PATH;
    e.role   = "upload";
    e.auth   = WebAuthLevel::Admin;
    page.endpoints.push_back(std::move(e));
  }
  web->registerPage(std::move(page));

  WebTabSpec tab;
  tab.key      = "upload";
  tab.title    = "Upload Firmware";
  tab.restPath = UPLOAD_FIRMWARE_FORM_PATH;
  tab.postable = false;
  tab.auth     = WebAuthLevel::Admin;
  tab.order    = 70;
  web->addTabToFeature("system", tab);
}

void UploadFirmwareService::buildForm(JsonObject& root) {
  JsonArray set = FormBuilder::createForm(root, "upload", "Upload Firmware");
  FormBuilder::addMessageField(set, "warn",
      "Upload a new firmware (.bin) file below to replace the existing firmware.",
      level("warning"), icon("Warning"));
  FormBuilder::addUploadField(set, "firmware", AF::RW,
      UPLOAD_FIRMWARE_PATH, "application/octet-stream",
      icon("CloudUpload"), color("warning"));
}

void UploadFirmwareService::uploadForm(AsyncWebServerRequest* request) {
  sendAdaptiveJson(request, [](JsonObject& root) {
    buildForm(root);
  }, 1024);
}

#ifdef ESP32

namespace {

const char* uploadFailure(ESPRack::UploadResult r) {
  using ESPRack::UploadResult;
  switch (r) {
    case UploadResult::Ok: return "";
    case UploadResult::SinkFailed: return "image write failed";
    case UploadResult::Overflow: return "upload buffer overflow";
    case UploadResult::NoMemory: return "no memory for the upload buffer";
    case UploadResult::NoWriter: return "flash writes held, no writer task";
    case UploadResult::Stalled: return "upload stalled";
    case UploadResult::Aborted: return "upload aborted";
  }
  return "upload failed";
}

}  // namespace

void UploadFirmwareService::handleUpload(AsyncWebServerRequest* request,
                                         const String& filename,
                                         size_t index,
                                         uint8_t* data,
                                         size_t len,
                                         bool final) {
  // refused earlier: already answered, the rest of the body is dropped
  if (request->_tempObject) return;
  if (!index && !_pump.owns(request)) {
    Authentication authentication = _securityManager->authenticateRequest(request);
    if (!AuthenticationPredicates::IS_ADMIN(authentication)) {
      handleError(request, 403);
      return;
    }
    // As Update.begin(contentLength) refused it: no slot, or the body
    // doesn't fit it (the image is a little smaller than the body).
    const uint32_t slot = ESPRack::OtaWriter::freeSlotSize();
    const size_t size = request->contentLength();
    if (!slot || size > slot) {
      log_e("[upload] firmware: %u B, OTA slot %u B", (unsigned)size, (unsigned)slot);
      handleError(request, 500);
      return;
    }
    // One image at a time: another upload, auto-update or ArduinoOTA. The
    // update is the upload's from here (open may wait for a slot); a
    // failed upload gives it back with the sink's abort. _ota is the last
    // upload's until its session has settled (its abort may still run).
    if (_pump.active() || !_ota.reserve("upload")) {
      handleError(request, 503);
      return;
    }
    if (!_pump.begin(request, _ota.sink("upload", static_cast<uint32_t>(size)))) {
      _ota.abort();  // nothing begun: the reservation only
      handleError(request, 503);
      return;
    }
  }
  // The pump keeps what it can't write now; a failure is answered in
  // uploadComplete. Not its request (a second file part after the first
  // one ended): dropped.
  (void)_pump.push(request, data, len, final);
}

void UploadFirmwareService::uploadComplete(AsyncWebServerRequest* request) {
  // handleUpload already answered with its error
  if (request->_tempObject) return;
  // The pump answers once the image is written and checked (at once, or
  // from its poll while the gate holds writes); the device restarts once
  // that client has gone, and the restart sets the image to boot (after
  // the modules' shutdown: OtaWriter::restartInto). A failure: the pump
  // aborts the image (the update is given back).
  const bool mine = _pump.finish(
      request,
      [this](AsyncWebServerRequest* req, ESPRack::UploadResult r) -> AsyncWebServerResponse* {
        if (r == ESPRack::UploadResult::Ok) return req->beginResponse(200);
        const char* why = r == ESPRack::UploadResult::SinkFailed && _ota.error() ? _ota.errorString()
                                                                                 : uploadFailure(r);
        log_e("[upload] firmware: %s", why);
        return req->beginResponse(500, "text/plain", why);
      },
      [this](ESPRack::UploadResult r) {
        if (r == ESPRack::UploadResult::Ok) _ota.restartInto(ESPRack::kRestartUpload);
      });
  if (mine) return;
  // Only a finished, authorised upload restarts the device. A POST without a
  // file part never reaches handleUpload, so it is refused here: 403 for a
  // caller who may not upload, 400 for an admin who sent no firmware.
  Authentication authentication = _securityManager->authenticateRequest(request);
  handleError(request, AuthenticationPredicates::IS_ADMIN(authentication) ? 400 : 403);
}

void UploadFirmwareService::handleError(AsyncWebServerRequest* request, int code) {
  // if we have had an error already, do nothing
  if (request->_tempObject) {
    return;
  }
  // send the error code to the client and record the error code in the temp
  // object (FirmwareUpdate is the OtaWriter's: this request began nothing)
  request->_tempObject = new int(code);
  AsyncWebServerResponse* response = request->beginResponse(code);
  request->send(response);
}

#else  // ESP8266: Arduino Update, as before

void UploadFirmwareService::handleUpload(AsyncWebServerRequest* request,
                                         const String& filename,
                                         size_t index,
                                         uint8_t* data,
                                         size_t len,
                                         bool final) {
  if (!index) {
    Authentication authentication = _securityManager->authenticateRequest(request);
    if (AuthenticationPredicates::IS_ADMIN(authentication)) {
      if (Update.begin(request->contentLength())) {
        ESPRack::FirmwareUpdate::begin("upload");
        // success, let's make sure we end the update if the client hangs up
        request->onDisconnect(UploadFirmwareService::handleEarlyDisconnect);
      } else {
        // failed to begin, send an error response
        Update.printError(Serial);
        handleError(request, 500);
      }
    } else {
      // send the forbidden response
      handleError(request, 403);
    }
  }

  // if we haven't delt with an error, continue with the update
  if (!request->_tempObject) {
    if (Update.write(data, len) != len) {
      Update.printError(Serial);
      handleError(request, 500);
    } else {
      ESPRack::FirmwareUpdate::progress();
    }
    if (final) {
      ESPRack::FirmwareUpdate::end();
      if (!Update.end(true)) {
        Update.printError(Serial);
        handleError(request, 500);
      } else {
        _finished = request;
      }
    }
  }
}

void UploadFirmwareService::uploadComplete(AsyncWebServerRequest* request) {
  // handleUpload already answered with its error
  if (request->_tempObject) return;
  // Only a finished, authorised upload restarts the device. A POST without a
  // file part never reaches handleUpload, so it is refused here: 403 for a
  // caller who may not upload, 400 for an admin who sent no firmware.
  const bool done = _finished == request;
  _finished = nullptr;
  if (!done) {
    Authentication authentication = _securityManager->authenticateRequest(request);
    handleError(request, AuthenticationPredicates::IS_ADMIN(authentication) ? 400 : 403);
    return;
  }
  request->onDisconnect([]() { ESPRack::restartDevice(ESPRack::kRestartUpload); });
  AsyncWebServerResponse* response = request->beginResponse(200);
  request->send(response);
}

void UploadFirmwareService::handleError(AsyncWebServerRequest* request, int code) {
  // if we have had an error already, do nothing
  if (request->_tempObject) {
    return;
  }
  // send the error code to the client and record the error code in the temp object
  ESPRack::FirmwareUpdate::end();
  request->_tempObject = new int(code);
  AsyncWebServerResponse* response = request->beginResponse(code);
  request->send(response);
}

void UploadFirmwareService::handleEarlyDisconnect() {
  ESPRack::FirmwareUpdate::end();
}

#endif  // ESP32
