#include <UploadFirmwareService.h>

#include <FormBuilder.h>
#include <WebManager.h>

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
#ifdef ESP8266
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
  AsyncJsonResponse* response = new AsyncJsonResponse(false, 2048);
  JsonObject root = response->getRoot();
  buildForm(root);
  response->setLength();
  request->send(response);
}

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
    }
    if (final) {
      if (!Update.end(true)) {
        Update.printError(Serial);
        handleError(request, 500);
      }
    }
  }
}

void UploadFirmwareService::uploadComplete(AsyncWebServerRequest* request) {
  // if no error, send the success response
  if (!request->_tempObject) {
    request->onDisconnect(RestartService::restartNow);
    AsyncWebServerResponse* response = request->beginResponse(200);
    request->send(response);
  }
}

void UploadFirmwareService::handleError(AsyncWebServerRequest* request, int code) {
  // if we have had an error already, do nothing
  if (request->_tempObject) {
    return;
  }
  // send the error code to the client and record the error code in the temp object
  request->_tempObject = new int(code);
  AsyncWebServerResponse* response = request->beginResponse(code);
  request->send(response);
}

void UploadFirmwareService::handleEarlyDisconnect() {
#ifdef ESP32
  Update.abort();
#endif
}
