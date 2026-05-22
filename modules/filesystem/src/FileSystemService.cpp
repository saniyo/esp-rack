#include "FileSystemService.h"

#include <FormBuilder.h>
#include <esp_rom_crc.h>
#include <mbedtls/version.h>

#include <WebManager.h>

// mbedTLS shipped with Arduino-ESP32 2.x exposed the SHA-256 streaming
// API as `_ret`-suffixed functions; mbedTLS ≥ 3 (used by Arduino 3.x /
// IDF 5) drops the suffix. Bridge both via macros so the same source
// builds on either core (S3 stays on the legacy core, C6 needs IDF 5).
#if defined(MBEDTLS_VERSION_NUMBER) && MBEDTLS_VERSION_NUMBER >= 0x03000000
#define COMPAT_SHA256_STARTS(ctx, is224)        mbedtls_sha256_starts((ctx), (is224))
#define COMPAT_SHA256_UPDATE(ctx, input, ilen)  mbedtls_sha256_update((ctx), (input), (ilen))
#define COMPAT_SHA256_FINISH(ctx, output)       mbedtls_sha256_finish((ctx), (output))
#else
#define COMPAT_SHA256_STARTS(ctx, is224)        mbedtls_sha256_starts_ret((ctx), (is224))
#define COMPAT_SHA256_UPDATE(ctx, input, ilen)  mbedtls_sha256_update_ret((ctx), (input), (ilen))
#define COMPAT_SHA256_FINISH(ctx, output)       mbedtls_sha256_finish_ret((ctx), (output))
#endif

namespace {
constexpr size_t kMaxUploadBuffer = 4096;  // reserved; upload is streamed, no buffering

String queryPath(AsyncWebServerRequest* request) {
  if (request->hasParam("path")) return request->getParam("path")->value();
  return String();
}

String headerValue(AsyncWebServerRequest* request, const char* name) {
  if (!request->hasHeader(name)) return String();
  return request->getHeader(name)->value();
}

String toLowerHex(const String& s) {
  String r = s;
  r.toLowerCase();
  return r;
}

String sha256ToHex(const uint8_t out[32]) {
  static const char* H = "0123456789abcdef";
  char buf[65];
  for (int i = 0; i < 32; i++) {
    buf[i * 2]     = H[(out[i] >> 4) & 0xF];
    buf[i * 2 + 1] = H[out[i] & 0xF];
  }
  buf[64] = 0;
  return String(buf);
}

String crc32ToHex(uint32_t crc) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%08x", (unsigned)crc);
  return String(buf);
}
}  // namespace

FileSystemService::FileSystemService(AsyncWebServer* server,
                                     FileSystemManager* manager,
                                     SecurityManager* securityManager)
    : _server(server), _manager(manager), _sec(securityManager) {
  using namespace std::placeholders;

  // Multipart upload is the only endpoint that doesn't fit
  // WebEndpointMeta's handler/jsonHandler/internalHandler trio —
  // it needs the 4-arg server->on(path, method, finalHandler,
  // bodyHandler) shape for chunked body delivery. Stays bound
  // directly to AsyncWebServer; the manifest's WebPageSpec still
  // lists it (without an internalHandler — caller surfaces 501
  // through proxyDispatch, which is honest: the operator should
  // hit the local /rest/fs/upload endpoint with a real HTTP client
  // for multipart, not try to wrap it in an action JSON envelope).
  _server->on(FS_UPLOAD_PATH, HTTP_POST,
              std::bind(&FileSystemService::handleUploadDone, this, _1),
              std::bind(&FileSystemService::handleUpload, this, _1, _2, _3, _4, _5, _6));
}

void FileSystemService::registerManifest(WebManager* web) {
  if (!web) return;
  using namespace std::placeholders;

  // Migration from raw server->on() to WebManager-driven binding:
  // each endpoint declared here gets its handler/jsonHandler bound by
  // WebPageEntry::registerEndpoints during web->begin(). No more
  // bypass through the AsyncWebServer ctor.
  //
  // Local-browser-UI semantics unchanged — same paths, same auth
  // predicates, same handlers. Bonus: Mothership's rest.proxy can
  // now reach these endpoints (Phase 2) once each handler is twinned
  // with an internalHandler. For this checkpoint the internalHandlers
  // are absent; rest.proxy returns 501 with a clear "no_internal_handler"
  // hint until the second pass fills them in.
  //
  // FS_UPLOAD_PATH is the one exception — its 4-arg multipart shape
  // doesn't fit WebEndpointMeta. Listed here for manifest visibility
  // but the actual binding stays in the ctor against AsyncWebServer.
  WebPageSpec page;
  page.id = "filesystem";
  page.title = "File System";
  page.component = "FileSystem";
  page.menu.label = "File System";
  page.menu.icon = "Folder";
  page.menu.order = 800;
  page.menu.auth = WebAuthLevel::Admin;
  page.auth = WebAuthLevel::Admin;

  // Helper closures to cut boilerplate — these capture `this` so the
  // member functions hit the right instance. Each `bind` retains the
  // method's argument shape (request* / request* + json variant).
  auto bindReq   = [this](void (FileSystemService::*m)(AsyncWebServerRequest*)) {
    return std::bind(m, this, _1);
  };
  auto bindReqJs = [this](void (FileSystemService::*m)(AsyncWebServerRequest*, JsonVariant&)) {
    return std::bind(m, this, _1, _2);
  };

  // ── GET endpoints (no body) ────────────────────────────────────────
  {
    WebEndpointMeta e;
    e.method = "GET"; e.path = FS_SCHEMA_PATH; e.role = "schema";
    e.auth = WebAuthLevel::Admin;
    e.handler = bindReq(&FileSystemService::handleSchema);
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "GET"; e.path = FS_VOLUMES_PATH; e.role = "volumes";
    e.auth = WebAuthLevel::Admin;
    e.handler = bindReq(&FileSystemService::handleVolumes);
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "GET"; e.path = FS_LIST_PATH; e.role = "list";
    e.auth = WebAuthLevel::Admin;
    e.handler = bindReq(&FileSystemService::handleList);
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "GET"; e.path = FS_STAT_PATH; e.role = "stat";
    e.auth = WebAuthLevel::Admin;
    e.handler = bindReq(&FileSystemService::handleStat);
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "GET"; e.path = FS_DOWNLOAD_PATH; e.role = "download";
    e.auth = WebAuthLevel::Admin;
    e.handler = bindReq(&FileSystemService::handleDownload);
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "GET"; e.path = FS_FORMAT_STATUS_PATH; e.role = "formatStatus";
    e.auth = WebAuthLevel::Admin;
    e.handler = bindReq(&FileSystemService::handleFormatStatus);
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "GET"; e.path = FS_ARCHIVE_STATUS_PATH; e.role = "archiveStatus";
    e.auth = WebAuthLevel::Admin;
    e.handler = bindReq(&FileSystemService::handleArchiveStatus);
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "GET"; e.path = FS_ARCHIVE_DOWNLOAD_PATH; e.role = "archiveDownload";
    e.auth = WebAuthLevel::Admin;
    e.handler = bindReq(&FileSystemService::handleArchiveDownload);
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "GET"; e.path = FS_HASH_STATUS_PATH; e.role = "hashStatus";
    e.auth = WebAuthLevel::Admin;
    e.handler = bindReq(&FileSystemService::handleHashStatus);
    page.endpoints.push_back(std::move(e));
  }

  // ── POST endpoints (JSON body, auto-parsed by AsyncCallbackJsonWebHandler) ─
  {
    WebEndpointMeta e;
    e.method = "POST"; e.path = FS_MKDIR_PATH; e.role = "mkdir";
    e.auth = WebAuthLevel::Admin;
    e.jsonHandler = bindReqJs(&FileSystemService::handleMkdir);
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "POST"; e.path = FS_DELETE_PATH; e.role = "delete";
    e.auth = WebAuthLevel::Admin;
    e.jsonHandler = bindReqJs(&FileSystemService::handleDelete);
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "POST"; e.path = FS_RENAME_PATH; e.role = "rename";
    e.auth = WebAuthLevel::Admin;
    e.jsonHandler = bindReqJs(&FileSystemService::handleRename);
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "POST"; e.path = FS_FORMAT_PATH; e.role = "format";
    e.auth = WebAuthLevel::Admin;
    e.jsonHandler = bindReqJs(&FileSystemService::handleFormat);
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "POST"; e.path = FS_UNMOUNT_PATH; e.role = "unmount";
    e.auth = WebAuthLevel::Admin;
    e.jsonHandler = bindReqJs(&FileSystemService::handleUnmount);
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "POST"; e.path = FS_MOUNT_PATH; e.role = "mount";
    e.auth = WebAuthLevel::Admin;
    e.jsonHandler = bindReqJs(&FileSystemService::handleMount);
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "POST"; e.path = FS_ARCHIVE_PREPARE_PATH; e.role = "archivePrepare";
    e.auth = WebAuthLevel::Admin;
    e.jsonHandler = bindReqJs(&FileSystemService::handleArchivePrepare);
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "POST"; e.path = FS_ARCHIVE_CANCEL_PATH; e.role = "archiveCancel";
    e.auth = WebAuthLevel::Admin;
    e.jsonHandler = bindReqJs(&FileSystemService::handleArchiveCancel);
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "POST"; e.path = FS_HASH_START_PATH; e.role = "hashStart";
    e.auth = WebAuthLevel::Admin;
    e.jsonHandler = bindReqJs(&FileSystemService::handleHashStart);
    page.endpoints.push_back(std::move(e));
  }
  {
    WebEndpointMeta e;
    e.method = "POST"; e.path = FS_HASH_CANCEL_PATH; e.role = "hashCancel";
    e.auth = WebAuthLevel::Admin;
    e.jsonHandler = bindReqJs(&FileSystemService::handleHashCancel);
    page.endpoints.push_back(std::move(e));
  }

  // FS_UPLOAD_PATH — multipart, bound in ctor; manifest-visible only.
  {
    WebEndpointMeta e;
    e.method = "POST"; e.path = FS_UPLOAD_PATH; e.role = "upload";
    e.auth = WebAuthLevel::Admin;
    // No handler set — WebPageEntry skips binding, ctor already bound
    // the multipart-aware version against AsyncWebServer.
    page.endpoints.push_back(std::move(e));
  }

  web->registerPage(std::move(page));
}

int FileSystemService::httpCodeFor(FsResult r) {
  switch (r) {
    case FsResult::OK: return 200;
    case FsResult::NOT_FOUND: return 404;
    case FsResult::NOT_MOUNTED: return 503;
    case FsResult::READ_ONLY: return 403;
    case FsResult::INVALID_PATH: return 400;
    case FsResult::EXISTS: return 409;
    case FsResult::IO_ERROR: return 500;
  }
  return 500;
}

void FileSystemService::sendError(AsyncWebServerRequest* request, int code, FsResult res) {
  DynamicJsonDocument doc(128);
  doc["ok"] = false;
  doc["error"] = FileSystemManager::resultString(res);
  String body;
  serializeJson(doc, body);
  request->send(code, "application/json", body);
}

void FileSystemService::sendError(AsyncWebServerRequest* request, int code, const char* msg) {
  DynamicJsonDocument doc(128);
  doc["ok"] = false;
  doc["error"] = msg;
  String body;
  serializeJson(doc, body);
  request->send(code, "application/json", body);
}

void FileSystemService::sendJson(AsyncWebServerRequest* request, int code, const JsonDocument& doc) {
  String body;
  serializeJson(doc, body);
  request->send(code, "application/json", body);
}

// --------- GET /schema ----------
void FileSystemService::handleSchema(AsyncWebServerRequest* request) {
  DynamicJsonDocument doc(1024);
  JsonObject root = doc.to<JsonObject>();
  JsonArray browser = FormBuilder::createForm(root, "browser", "File System");
  FormBuilder::addFilesField(browser, "path", AF::RW, FS_REST_BASE, "/flash", FS_UPLOAD_PATH);
  sendJson(request, 200, doc);
}

// --------- GET /volumes ----------
void FileSystemService::handleVolumes(AsyncWebServerRequest* request) {
  const auto& backends = _manager->backends();
  DynamicJsonDocument doc(512 + backends.size() * 128);
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& b : backends) {
    JsonObject v = arr.createNestedObject();
    v["name"] = b->name();
    v["mount"] = String("/") + b->name();
    v["mounted"] = b->isMounted();
    v["readOnly"] = b->readOnly();
    v["canFormat"] = b->canFormat();
    v["total"] = (double)b->totalBytes();
    v["used"] = (double)b->usedBytes();
  }
  sendJson(request, 200, doc);
}

// --------- GET /list ----------
void FileSystemService::handleList(AsyncWebServerRequest* request) {
  String path = queryPath(request);
  if (path.length() == 0) { sendError(request, 400, "missing_path"); return; }

  // Root "/" — return empty entries (volumes shown separately)
  if (path == "/") {
    DynamicJsonDocument doc(128);
    JsonObject root = doc.to<JsonObject>();
    root["path"] = "/";
    root["readOnly"] = true;
    root.createNestedArray("entries");
    sendJson(request, 200, doc);
    return;
  }

  std::vector<FileSystemEntry> entries;
  FsResult r = _manager->list(path, entries);
  if (r != FsResult::OK) { sendError(request, httpCodeFor(r), r); return; }

  DynamicJsonDocument doc(512 + entries.size() * 128);
  JsonObject root = doc.to<JsonObject>();
  root["path"] = path;
  root["readOnly"] = _manager->isReadOnlyPath(path);
  JsonArray arr = root.createNestedArray("entries");
  for (const auto& e : entries) {
    JsonObject o = arr.createNestedObject();
    o["name"] = e.name;
    o["isDir"] = e.isDir;
    o["size"] = (double)e.size;
    o["mtime"] = e.mtime;
  }
  sendJson(request, 200, doc);
}

// --------- GET /stat ----------
void FileSystemService::handleStat(AsyncWebServerRequest* request) {
  String path = queryPath(request);
  if (path.length() == 0) { sendError(request, 400, "missing_path"); return; }
  FileSystemEntry e;
  FsResult r = _manager->stat(path, e);
  if (r != FsResult::OK) { sendError(request, httpCodeFor(r), r); return; }

  DynamicJsonDocument doc(256);
  JsonObject root = doc.to<JsonObject>();
  root["name"] = e.name;
  root["isDir"] = e.isDir;
  root["size"] = (double)e.size;
  root["mtime"] = e.mtime;
  sendJson(request, 200, doc);
}

// --------- GET /download ----------
void FileSystemService::handleDownload(AsyncWebServerRequest* request) {
  String path = queryPath(request);
  if (path.length() == 0) { sendError(request, 400, "missing_path"); return; }

  String rel;
  FileSystemBackend* b = _manager->resolve(path, rel);
  if (!b) { sendError(request, 400, FsResult::INVALID_PATH); return; }
  if (!b->isMounted()) { sendError(request, 503, FsResult::NOT_MOUNTED); return; }
  fs::FS* fs = b->fs();
  if (!fs || !fs->exists(rel)) { sendError(request, 404, FsResult::NOT_FOUND); return; }

  AsyncWebServerResponse* response = request->beginResponse(*fs, rel, "application/octet-stream", true);
  // Content-Disposition for browser download
  const char* slash = strrchr(rel.c_str(), '/');
  String filename = slash ? String(slash + 1) : rel;
  response->addHeader("Content-Disposition", String("attachment; filename=\"") + filename + "\"");
  request->send(response);
}

// --------- POST /upload (multipart) ----------
// Query param: ?path=/flash/foo/ (target directory)
// Client-provided filename is appended to target directory.
void FileSystemService::handleUpload(AsyncWebServerRequest* request,
                                     const String& filename,
                                     size_t index,
                                     uint8_t* data,
                                     size_t len,
                                     bool final) {
  if (!index) {
    _uploadFailed = false;
    if (_uploadFile) _uploadFile.close();
    if (_sha256CtxInit) {
      mbedtls_sha256_free(&_sha256Ctx);
      _sha256CtxInit = false;
    }
    _hashEnabled = false;
    _crc = 0;
    _expectedSha256 = "";
    _expectedCrc32 = "";
    _uploadTarget = "";

    Authentication auth = _sec->authenticateRequest(request);
    if (!AuthenticationPredicates::IS_ADMIN(auth)) {
      sendError(request, 403, "forbidden");
      _uploadFailed = true;
      return;
    }

    String target = queryPath(request);
    if (target.length() == 0) {
      sendError(request, 400, "missing_path");
      _uploadFailed = true;
      return;
    }
    if (!target.endsWith("/")) target += "/";
    target += filename;

    if (_manager->isReadOnlyPath(target)) {
      sendError(request, 403, FsResult::READ_ONLY);
      _uploadFailed = true;
      return;
    }

    FsResult r;
    _uploadFile = _manager->openWrite(target, r);
    if (!_uploadFile) {
      sendError(request, httpCodeFor(r), r);
      _uploadFailed = true;
      return;
    }
    _uploadTarget = target;
    log_d("[upload] start: %s", target.c_str());

    // Optional integrity validation (opt-in via headers).
    _expectedSha256 = toLowerHex(headerValue(request, "X-File-SHA256"));
    _expectedCrc32 = toLowerHex(headerValue(request, "X-File-CRC32"));
    if (_expectedSha256.length() == 64 || _expectedCrc32.length() == 8) {
      _hashEnabled = true;
      mbedtls_sha256_init(&_sha256Ctx);
      COMPAT_SHA256_STARTS(&_sha256Ctx, 0);
      _sha256CtxInit = true;
      _crc = 0;
    }

    // ALWAYS register disconnect cleanup — independent of hash validation —
    // so any abort (tab-switch, TCP drop, browser close) salvages the file.
    // Capture target by value so the lambda is correct even if a later upload
    // overwrites _uploadTarget before this disconnect fires.
    String capturedTarget = target;
    request->onDisconnect([this, capturedTarget]() {
      // Only act if our upload is still the in-flight one. If _uploadTarget
      // already differs (cleared by successful final, or replaced by a later
      // upload) we must not touch shared state or re-rename a now-valid file.
      if (_uploadTarget != capturedTarget) {
        return;  // happy-path post-success disconnect; stay silent
      }
      log_d("[upload] disconnect (interrupted): %s", capturedTarget.c_str());

      if (_uploadFile) _uploadFile.close();
      if (_sha256CtxInit) {
        mbedtls_sha256_free(&_sha256Ctx);
        _sha256CtxInit = false;
      }
      _hashEnabled = false;
      _uploadTarget = "";

      String partial = capturedTarget + ".partial";
      _manager->remove(partial, false);
      FsResult rr = _manager->rename(capturedTarget, partial);
      log_d("[upload] salvage rename %s -> .partial : %s",
                    capturedTarget.c_str(), FileSystemManager::resultString(rr));
    });
  }

  if (_uploadFailed) return;

  if (len && _uploadFile) {
    if (_uploadFile.write(data, len) != len) {
      _uploadFile.close();
      sendError(request, 500, FsResult::IO_ERROR);
      _uploadFailed = true;
      return;
    }
    if (_hashEnabled) {
      if (_sha256CtxInit) COMPAT_SHA256_UPDATE(&_sha256Ctx, data, len);
      _crc = esp_rom_crc32_le(_crc, data, len);
    }
  }

  if (final && _uploadFile) {
    _uploadFile.close();

    if (_hashEnabled) {
      uint8_t out[32];
      String actualSha;
      if (_sha256CtxInit) {
        COMPAT_SHA256_FINISH(&_sha256Ctx, out);
        mbedtls_sha256_free(&_sha256Ctx);
        _sha256CtxInit = false;
        actualSha = sha256ToHex(out);
      }
      String actualCrc = crc32ToHex(_crc);

      bool shaOk = _expectedSha256.length() == 64 ? actualSha == _expectedSha256 : true;
      bool crcOk = _expectedCrc32.length() == 8 ? actualCrc == _expectedCrc32 : true;

      if (!(shaOk && crcOk)) {
        // Preserve the corrupt upload for inspection rather than silently deleting.
        String corruptPath = _uploadTarget + ".corrupt";
        _manager->remove(corruptPath, false);  // clear any stale .corrupt from prior attempt
        _manager->rename(_uploadTarget, corruptPath);

        DynamicJsonDocument doc(384);
        doc["ok"] = false;
        doc["error"] = "hash_mismatch";
        if (_expectedSha256.length() == 64) {
          doc["expectedSha256"] = _expectedSha256;
          doc["actualSha256"] = actualSha;
        }
        if (_expectedCrc32.length() == 8) {
          doc["expectedCrc32"] = _expectedCrc32;
          doc["actualCrc32"] = actualCrc;
        }
        doc["savedAs"] = corruptPath;
        sendJson(request, 422, doc);
        _uploadFailed = true;
        _hashEnabled = false;
        _uploadTarget = "";
        return;
      }
      _hashEnabled = false;
    }
    _uploadTarget = "";
  }
}

void FileSystemService::handleUploadDone(AsyncWebServerRequest* request) {
  if (_uploadFailed) {
    _uploadFailed = false;
    return;  // error already responded
  }
  // Safety net: if the multipart parser handed us back without ever calling
  // handleUpload with final=true, _uploadTarget will still be set. Salvage.
  if (_uploadTarget.length() > 0) {
    log_i("[upload] done-without-final, salvaging: %s", _uploadTarget.c_str());
    if (_uploadFile) _uploadFile.close();
    if (_sha256CtxInit) {
      mbedtls_sha256_free(&_sha256Ctx);
      _sha256CtxInit = false;
    }
    _hashEnabled = false;
    String partial = _uploadTarget + ".partial";
    _manager->remove(partial, false);
    FsResult rr = _manager->rename(_uploadTarget, partial);
    log_d("[upload] safety-net rename: %s", FileSystemManager::resultString(rr));
    _uploadTarget = "";
    sendError(request, 500, "incomplete_upload");
    return;
  }
  DynamicJsonDocument doc(64);
  doc["ok"] = true;
  sendJson(request, 200, doc);
}

// --------- POST /mkdir ----------
void FileSystemService::handleMkdir(AsyncWebServerRequest* request, JsonVariant& json) {
  if (!json.is<JsonObject>()) { sendError(request, 400, "invalid_body"); return; }
  JsonObject o = json.as<JsonObject>();
  if (!o.containsKey("path")) { sendError(request, 400, "missing_path"); return; }
  FsResult r = _manager->mkdir(o["path"].as<const char*>());
  if (r != FsResult::OK) { sendError(request, httpCodeFor(r), r); return; }
  DynamicJsonDocument doc(32);
  doc["ok"] = true;
  sendJson(request, 200, doc);
}

// --------- POST /delete ----------
void FileSystemService::handleDelete(AsyncWebServerRequest* request, JsonVariant& json) {
  if (!json.is<JsonObject>()) { sendError(request, 400, "invalid_body"); return; }
  JsonObject o = json.as<JsonObject>();
  if (!o.containsKey("path")) { sendError(request, 400, "missing_path"); return; }
  bool recursive = o["recursive"] | false;
  FsResult r = _manager->remove(o["path"].as<const char*>(), recursive);
  if (r != FsResult::OK) { sendError(request, httpCodeFor(r), r); return; }
  DynamicJsonDocument doc(32);
  doc["ok"] = true;
  sendJson(request, 200, doc);
}

// --------- POST /rename ----------
void FileSystemService::handleRename(AsyncWebServerRequest* request, JsonVariant& json) {
  if (!json.is<JsonObject>()) { sendError(request, 400, "invalid_body"); return; }
  JsonObject o = json.as<JsonObject>();
  if (!o.containsKey("from") || !o.containsKey("to")) {
    sendError(request, 400, "missing_from_or_to");
    return;
  }
  FsResult r = _manager->rename(o["from"].as<const char*>(), o["to"].as<const char*>());
  if (r != FsResult::OK) { sendError(request, httpCodeFor(r), r); return; }
  DynamicJsonDocument doc(32);
  doc["ok"] = true;
  sendJson(request, 200, doc);
}

// --------- POST /format ----------
// --------- GET /format/status?volume=sd ----------
void FileSystemService::handleFormatStatus(AsyncWebServerRequest* request) {
  if (!request->hasParam("volume")) { sendError(request, 400, "missing_volume"); return; }
  String volume = request->getParam("volume")->value();

  FileSystemBackend* target = nullptr;
  for (const auto& b : _manager->backends()) {
    if (volume == b->name()) { target = b.get(); break; }
  }
  if (!target) { sendError(request, 404, "volume_not_found"); return; }

  DynamicJsonDocument doc(192);
  uint8_t st = target->formatState();
  const char* stStr = "idle";
  switch (st) {
    case 1: stStr = "counting"; break;
    case 2: stStr = "running"; break;
    case 3: stStr = "done"; break;
    case 4: stStr = "error"; break;
  }
  doc["state"] = stStr;
  doc["percent"] = target->formatProgress();
  doc["total"] = target->formatTotal();
  doc["done"] = target->formatDone();
  sendJson(request, 200, doc);
}

// Body: { "volume": "sd", "confirm": true }
// Double protection: confirm must be true + IS_ADMIN
void FileSystemService::handleFormat(AsyncWebServerRequest* request, JsonVariant& json) {
  if (!json.is<JsonObject>()) { sendError(request, 400, "invalid_body"); return; }
  JsonObject o = json.as<JsonObject>();

  const char* volume = o["volume"] | "";
  if (!volume[0]) { sendError(request, 400, "missing_volume"); return; }

  bool confirm = o["confirm"] | false;
  if (!confirm) { sendError(request, 400, "confirm_required"); return; }

  // find backend
  const auto& backends = _manager->backends();
  FileSystemBackend* target = nullptr;
  for (const auto& b : backends) {
    if (strcmp(b->name(), volume) == 0) { target = b.get(); break; }
  }
  if (!target) { sendError(request, 404, "volume_not_found"); return; }
  if (!target->isMounted()) { sendError(request, 503, FsResult::NOT_MOUNTED); return; }
  if (!target->canFormat()) { sendError(request, 403, "format_not_supported"); return; }

  // block formatting flash — too dangerous
  if (strcmp(volume, "flash") == 0) {
    sendError(request, 403, "cannot_format_flash");
    return;
  }

  // Format synchronously blocks async_tcp → triggers WDT on cards with many files.
  // Spawn a background FreeRTOS task; respond 202 Accepted immediately.
  FileSystemBackend* backendPtr = target;
  TaskHandle_t formatTask = nullptr;
  BaseType_t r = xTaskCreatePinnedToCore(
      [](void* arg) {
        auto* be = static_cast<FileSystemBackend*>(arg);
        log_i("[fs] format task started for volume='%s'", be->name());
        bool ok = be->format();
        log_i("[fs] format task done, ok=%d", (int)ok);
        vTaskDelete(nullptr);
      },
      "fs_format",
      4096,             // stack
      backendPtr,       // arg
      1,                // low priority — must not starve async_tcp
      &formatTask,
      0                 // pin to core 0 (same as async_tcp; FreeRTOS will time-slice)
  );

  DynamicJsonDocument doc(96);
  if (r == pdPASS) {
    doc["ok"] = true;
    doc["status"] = "started";
    sendJson(request, 202, doc);
  } else {
    doc["ok"] = false;
    doc["error"] = "task_create_failed";
    sendJson(request, 500, doc);
  }
}

// --------- POST /unmount ----------
// Body: { "volume": "sd" }
void FileSystemService::handleUnmount(AsyncWebServerRequest* request, JsonVariant& json) {
  if (!json.is<JsonObject>()) { sendError(request, 400, "invalid_body"); return; }
  JsonObject o = json.as<JsonObject>();
  const char* volume = o["volume"] | "";
  if (!volume[0]) { sendError(request, 400, "missing_volume"); return; }

  // block unmounting flash — system will crash
  if (strcmp(volume, "flash") == 0) {
    sendError(request, 403, "cannot_unmount_flash");
    return;
  }

  const auto& backends = _manager->backends();
  FileSystemBackend* target = nullptr;
  for (const auto& b : backends) {
    if (strcmp(b->name(), volume) == 0) { target = b.get(); break; }
  }
  if (!target) { sendError(request, 404, "volume_not_found"); return; }
  if (!target->isMounted()) { sendError(request, 200, "already_unmounted"); return; }

  target->unmount();
  log_i("[FS] volume '%s' unmounted — safe to remove", volume);

  DynamicJsonDocument doc(32);
  doc["ok"] = true;
  sendJson(request, 200, doc);
}

// --------- POST /mount ----------
// Body: { "volume": "sd" }
void FileSystemService::handleMount(AsyncWebServerRequest* request, JsonVariant& json) {
  if (!json.is<JsonObject>()) { sendError(request, 400, "invalid_body"); return; }
  JsonObject o = json.as<JsonObject>();
  const char* volume = o["volume"] | "";
  if (!volume[0]) { sendError(request, 400, "missing_volume"); return; }

  const auto& backends = _manager->backends();
  FileSystemBackend* target = nullptr;
  for (const auto& b : backends) {
    if (strcmp(b->name(), volume) == 0) { target = b.get(); break; }
  }
  if (!target) { sendError(request, 404, "volume_not_found"); return; }
  if (target->isMounted()) {
    DynamicJsonDocument doc(32);
    doc["ok"] = true;
    sendJson(request, 200, doc);
    return;
  }

  // Non-blocking: kicks a background retry loop. UI re-checks /volumes
  // and will see `mounted: true` once the watcher succeeds.
  bool kicked = target->mountAsync();
  DynamicJsonDocument doc(64);
  doc["ok"] = kicked;
  doc["status"] = target->isMounted() ? "mounted" : "starting";
  if (!kicked) doc["error"] = "mount_failed";
  sendJson(request, kicked ? 202 : 500, doc);
}

// ===================== ARCHIVE =====================

namespace {
const char* archiveStateString(FileSystemService::ArchiveJob::State s) {
  using S = FileSystemService::ArchiveJob::State;
  switch (s) {
    case S::Idle:      return "idle";
    case S::Walking:   return "walking";
    case S::Ready:     return "ready";
    case S::Streaming: return "streaming";
    case S::Error:     return "error";
  }
  return "idle";
}
}  // namespace

// Walks the directory tree, populates job->entries.
// Runs as a FreeRTOS task, yields between entries to keep WDT happy.
static void archiveWalkerTask(void* arg) {
  using S = FileSystemService::ArchiveJob::State;
  auto* job = static_cast<FileSystemService::ArchiveJob*>(arg);

  log_i("[fs] archive walker started for '%s'", job->path.c_str());

  // Strip "/<volume>/" prefix to get path inside the backend's fs.
  String full = job->path;
  while (full.startsWith("/")) full.remove(0, 1);
  int firstSlash = full.indexOf('/');
  String relRoot = firstSlash >= 0 ? full.substring(firstSlash) : "/";
  if (relRoot.length() == 0) relRoot = "/";

  // Archive root name = last path segment.
  String archRoot = job->archiveName;

  std::vector<std::pair<String, String>> stack;  // (fs path, archive path)
  stack.emplace_back(relRoot, archRoot);

  constexpr size_t kMaxEntries = 4096;
  uint32_t totalBytes = 0;

  while (!stack.empty()) {
    if (job->cancelRequested) { job->error = "cancelled"; job->state = S::Error; goto cleanup; }
    if (job->entries.size() >= kMaxEntries) {
      job->error = "too_many_entries";
      job->state = S::Error;
      goto cleanup;
    }

    auto cur = stack.back(); stack.pop_back();
    const String& curFs = cur.first;
    const String& curArc = cur.second;

    TarStream::Entry de;
    de.archiveName = curArc + "/";
    de.fsPath = curFs;
    de.isDir = true;
    de.size = 0;
    de.mtime = 0;
    job->entries.push_back(std::move(de));
    job->walkedCount = job->entries.size();

    File dir = job->fs->open(curFs);
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      vTaskDelay(1);
      continue;
    }

    File ent = dir.openNextFile();
    while (ent) {
      if (job->cancelRequested) { ent.close(); dir.close(); job->error = "cancelled"; job->state = S::Error; goto cleanup; }
      String name = ent.name();
      const char* slash = strrchr(name.c_str(), '/');
      String base = slash ? String(slash + 1) : name;
      String childFs = curFs.endsWith("/") ? (curFs + base) : (curFs + "/" + base);
      String childArc = curArc + "/" + base;

      if (ent.isDirectory()) {
        ent.close();
        stack.emplace_back(childFs, childArc);
      } else {
        TarStream::Entry fe;
        fe.archiveName = childArc;
        fe.fsPath = childFs;
        fe.isDir = false;
        fe.size = ent.size();
        fe.mtime = ent.getLastWrite();
        totalBytes += fe.size;
        ent.close();
        job->entries.push_back(std::move(fe));
        job->walkedCount = job->entries.size();
      }
      vTaskDelay(1);
      ent = dir.openNextFile();
    }
    dir.close();
    vTaskDelay(1);
  }

  job->totalBytes = totalBytes;
  job->state = S::Ready;
  log_i("[fs] archive walker done: %u entries, %u bytes",
                (unsigned)job->entries.size(), (unsigned)totalBytes);

cleanup:
  vTaskDelete(nullptr);
}

// POST /rest/fs/archive/prepare  body: {"path": "/flash/foo"}
void FileSystemService::handleArchivePrepare(AsyncWebServerRequest* request, JsonVariant& json) {
  using S = ArchiveJob::State;

  if (_archive.state == S::Walking || _archive.state == S::Streaming) {
    sendError(request, 409, "archive_busy");
    return;
  }

  if (!json.is<JsonObject>()) { sendError(request, 400, "invalid_body"); return; }
  JsonObject o = json.as<JsonObject>();
  const char* path = o["path"] | "";
  if (!path[0]) { sendError(request, 400, "missing_path"); return; }

  String rel;
  FileSystemBackend* b = _manager->resolve(path, rel);
  if (!b) { sendError(request, 400, FsResult::INVALID_PATH); return; }
  if (!b->isMounted()) { sendError(request, 503, FsResult::NOT_MOUNTED); return; }
  fs::FS* fs = b->fs();
  if (!fs || !fs->exists(rel)) { sendError(request, 404, FsResult::NOT_FOUND); return; }
  File f = fs->open(rel);
  bool isDir = f && f.isDirectory();
  if (f) f.close();
  if (!isDir) { sendError(request, 400, "not_a_directory"); return; }

  // Reset state
  _archive.entries.clear();
  _archive.entries.shrink_to_fit();
  _archive.streamer.reset();
  _archive.error = "";
  _archive.walkedCount = 0;
  _archive.totalBytes = 0;
  _archive.cancelRequested = false;
  _archive.path = String(path);
  _archive.fs = fs;

  // Derive archive root name from last path segment (or volume name for "/<volume>")
  String p = String(path);
  while (p.endsWith("/") && p.length() > 1) p.remove(p.length() - 1);
  int s = p.lastIndexOf('/');
  String last = (s >= 0 && s < (int)p.length() - 1) ? p.substring(s + 1) : p;
  if (last.length() == 0) last = b->name();
  _archive.archiveName = last;
  _archive.state = S::Walking;

  TaskHandle_t th = nullptr;
  BaseType_t r = xTaskCreatePinnedToCore(
      archiveWalkerTask, "fs_archive_walk", 6144, &_archive, 1, &th, 0);

  DynamicJsonDocument doc(96);
  if (r != pdPASS) {
    _archive.state = S::Error;
    _archive.error = "task_create_failed";
    doc["ok"] = false;
    doc["error"] = "task_create_failed";
    sendJson(request, 500, doc);
    return;
  }
  doc["ok"] = true;
  doc["status"] = "started";
  sendJson(request, 202, doc);
}

// GET /rest/fs/archive/status
void FileSystemService::handleArchiveStatus(AsyncWebServerRequest* request) {
  DynamicJsonDocument doc(192);
  doc["state"] = archiveStateString(_archive.state);
  doc["walked"] = _archive.walkedCount;
  doc["totalBytes"] = _archive.totalBytes;
  doc["entries"] = (uint32_t)_archive.entries.size();
  doc["path"] = _archive.path;
  doc["archiveName"] = _archive.archiveName;
  if (_archive.state == ArchiveJob::State::Error) doc["error"] = _archive.error;
  sendJson(request, 200, doc);
}

// GET /rest/fs/archive/download — streams the prepared TAR
void FileSystemService::handleArchiveDownload(AsyncWebServerRequest* request) {
  using S = ArchiveJob::State;
  if (_archive.state != S::Ready) { sendError(request, 409, "archive_not_ready"); return; }
  if (!_archive.fs) { sendError(request, 500, "archive_no_fs"); return; }
  if (_archive.entries.empty()) { sendError(request, 500, "archive_empty"); return; }

  _archive.streamer.reset(new TarStream(_archive.fs, std::move(_archive.entries)));
  _archive.entries.clear();
  _archive.entries.shrink_to_fit();
  _archive.state = S::Streaming;

  TarStream* s = _archive.streamer.get();
  AsyncWebServerResponse* response = request->beginChunkedResponse(
      "application/x-tar",
      [s](uint8_t* buf, size_t maxLen, size_t /*index*/) -> size_t {
        return s->fill(buf, maxLen);
      });
  String filename = _archive.archiveName + ".tar";
  response->addHeader("Content-Disposition", String("attachment; filename=\"") + filename + "\"");

  // Free job state once the response is fully sent (or aborted).
  request->onDisconnect([this]() {
    _archive.streamer.reset();
    _archive.state = ArchiveJob::State::Idle;
    _archive.walkedCount = 0;
    _archive.totalBytes = 0;
    _archive.path = "";
    _archive.archiveName = "";
    _archive.fs = nullptr;
  });

  request->send(response);
}

// POST /rest/fs/archive/cancel
void FileSystemService::handleArchiveCancel(AsyncWebServerRequest* request, JsonVariant& /*json*/) {
  using S = ArchiveJob::State;
  if (_archive.state == S::Walking) {
    _archive.cancelRequested = true;
  } else if (_archive.state == S::Ready) {
    _archive.entries.clear();
    _archive.entries.shrink_to_fit();
    _archive.state = S::Idle;
  }
  // Streaming cancel happens via TCP disconnect — onDisconnect cleans up.
  DynamicJsonDocument doc(32);
  doc["ok"] = true;
  sendJson(request, 200, doc);
}

// ===================== HASH (per-file verify) =====================

namespace {
const char* hashStateString(FileSystemService::HashJob::State s) {
  using S = FileSystemService::HashJob::State;
  switch (s) {
    case S::Idle:    return "idle";
    case S::Running: return "running";
    case S::Done:    return "done";
    case S::Error:   return "error";
  }
  return "idle";
}
}  // namespace

// Streams the file in 4 KB chunks, hashing with HW SHA-256 + ROM CRC32.
// vTaskDelay(1) between chunks keeps async_tcp and TWDT happy.
static void hashTask(void* arg) {
  using S = FileSystemService::HashJob::State;
  auto* job = static_cast<FileSystemService::HashJob*>(arg);

  log_i("[fs] hash task started for '%s'", job->path.c_str());

  if (!job->fs) {
    job->error = "no_fs";
    job->state = S::Error;
    vTaskDelete(nullptr);
    return;
  }

  File f = job->fs->open(job->fsPath, "r");
  if (!f) {
    job->error = "open_failed";
    job->state = S::Error;
    vTaskDelete(nullptr);
    return;
  }
  if (f.isDirectory()) {
    f.close();
    job->error = "is_directory";
    job->state = S::Error;
    vTaskDelete(nullptr);
    return;
  }

  job->total = f.size();
  job->bytesRead = 0;

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  COMPAT_SHA256_STARTS(&ctx, 0);
  uint32_t crc = 0;

  // 4 KB on stack of a 4 KB-stack task is too much; this task is created with 6 KB.
  static uint8_t buf[4096];  // single hash job at a time, so static is safe

  while (true) {
    if (job->cancelRequested) {
      mbedtls_sha256_free(&ctx);
      f.close();
      job->error = "cancelled";
      job->state = S::Idle;
      vTaskDelete(nullptr);
      return;
    }
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    COMPAT_SHA256_UPDATE(&ctx, buf, (size_t)n);
    crc = esp_rom_crc32_le(crc, buf, (uint32_t)n);
    job->bytesRead += (uint32_t)n;
    vTaskDelay(1);
  }

  uint8_t out[32];
  COMPAT_SHA256_FINISH(&ctx, out);
  mbedtls_sha256_free(&ctx);
  f.close();

  job->sha256Hex = sha256ToHex(out);
  job->crc32Hex = crc32ToHex(crc);
  job->state = S::Done;
  log_i("[fs] hash done: sha=%s crc=%s", job->sha256Hex.c_str(), job->crc32Hex.c_str());
  vTaskDelete(nullptr);
}

// POST /rest/fs/hash/start  body: {"path": "/sd/music/01.flac"}
void FileSystemService::handleHashStart(AsyncWebServerRequest* request, JsonVariant& json) {
  using S = HashJob::State;
  if (_hash.state == S::Running) { sendError(request, 409, "hash_busy"); return; }

  if (!json.is<JsonObject>()) { sendError(request, 400, "invalid_body"); return; }
  JsonObject o = json.as<JsonObject>();
  const char* path = o["path"] | "";
  if (!path[0]) { sendError(request, 400, "missing_path"); return; }

  String rel;
  FileSystemBackend* b = _manager->resolve(path, rel);
  if (!b) { sendError(request, 400, FsResult::INVALID_PATH); return; }
  if (!b->isMounted()) { sendError(request, 503, FsResult::NOT_MOUNTED); return; }
  fs::FS* fs = b->fs();
  if (!fs || !fs->exists(rel)) { sendError(request, 404, FsResult::NOT_FOUND); return; }

  _hash.cancelRequested = false;
  _hash.bytesRead = 0;
  _hash.total = 0;
  _hash.sha256Hex = "";
  _hash.crc32Hex = "";
  _hash.error = "";
  _hash.path = String(path);
  _hash.fsPath = rel;
  _hash.fs = fs;
  _hash.state = S::Running;

  TaskHandle_t th = nullptr;
  BaseType_t r = xTaskCreatePinnedToCore(
      hashTask, "fs_hash", 6144, &_hash, 1, &th, 0);

  DynamicJsonDocument doc(96);
  if (r != pdPASS) {
    _hash.state = S::Error;
    _hash.error = "task_create_failed";
    doc["ok"] = false;
    doc["error"] = "task_create_failed";
    sendJson(request, 500, doc);
    return;
  }
  doc["ok"] = true;
  doc["status"] = "started";
  sendJson(request, 202, doc);
}

// GET /rest/fs/hash/status
void FileSystemService::handleHashStatus(AsyncWebServerRequest* request) {
  DynamicJsonDocument doc(384);
  doc["state"] = hashStateString(_hash.state);
  doc["bytesRead"] = _hash.bytesRead;
  doc["total"] = _hash.total;
  uint32_t pct = 0;
  if (_hash.total > 0) {
    pct = (uint32_t)((uint64_t)_hash.bytesRead * 100ULL / (uint64_t)_hash.total);
    if (pct > 100) pct = 100;
  } else if (_hash.state == HashJob::State::Done) {
    pct = 100;
  }
  doc["percent"] = pct;
  doc["path"] = _hash.path;
  if (_hash.state == HashJob::State::Done) {
    doc["sha256"] = _hash.sha256Hex;
    doc["crc32"] = _hash.crc32Hex;
  }
  if (_hash.state == HashJob::State::Error) doc["error"] = _hash.error;
  sendJson(request, 200, doc);
}

// POST /rest/fs/hash/cancel
void FileSystemService::handleHashCancel(AsyncWebServerRequest* request, JsonVariant& /*json*/) {
  if (_hash.state == HashJob::State::Running) {
    _hash.cancelRequested = true;
  }
  DynamicJsonDocument doc(32);
  doc["ok"] = true;
  sendJson(request, 200, doc);
}
