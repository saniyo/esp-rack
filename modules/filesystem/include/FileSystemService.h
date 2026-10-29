#ifndef FileSystemService_h
#define FileSystemService_h

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <mbedtls/sha256.h>
#include <net/UploadPump.h>

#include <memory>

#include "FileSystemManager.h"
#include "SecurityManager.h"
#include "TarStream.h"

class WebManager;

#define FS_REST_BASE "/api/v1/fs"
#define FS_SCHEMA_PATH    FS_REST_BASE "/schema"
#define FS_VOLUMES_PATH   FS_REST_BASE "/volumes"
#define FS_LIST_PATH      FS_REST_BASE "/list"
#define FS_STAT_PATH      FS_REST_BASE "/stat"
#define FS_DOWNLOAD_PATH  FS_REST_BASE "/download"
#define FS_UPLOAD_PATH    FS_REST_BASE "/upload"
#define FS_MKDIR_PATH     FS_REST_BASE "/mkdir"
#define FS_DELETE_PATH    FS_REST_BASE "/delete"
#define FS_RENAME_PATH    FS_REST_BASE "/rename"
#define FS_FORMAT_PATH        FS_REST_BASE "/format"
#define FS_FORMAT_STATUS_PATH FS_REST_BASE "/format/status"
#define FS_UNMOUNT_PATH       FS_REST_BASE "/unmount"
#define FS_MOUNT_PATH         FS_REST_BASE "/mount"
#define FS_ARCHIVE_PREPARE_PATH  FS_REST_BASE "/archive/prepare"
#define FS_ARCHIVE_STATUS_PATH   FS_REST_BASE "/archive/status"
#define FS_ARCHIVE_DOWNLOAD_PATH FS_REST_BASE "/archive/download"
#define FS_ARCHIVE_CANCEL_PATH   FS_REST_BASE "/archive/cancel"
#define FS_HASH_START_PATH       FS_REST_BASE "/hash/start"
#define FS_HASH_STATUS_PATH      FS_REST_BASE "/hash/status"
#define FS_HASH_CANCEL_PATH      FS_REST_BASE "/hash/cancel"
#define FS_WRITE_STATUS_PATH     FS_REST_BASE "/write/status"

class FileSystemService {
 public:
  FileSystemService(AsyncWebServer* server,
                    FileSystemManager* manager,
                    SecurityManager* securityManager);

  // Register this service's 28 endpoints with the WebManager so the frontend
  // can resolve URLs by role from /api/v1/ui/manifest. Route handlers remain
  // bound by the ctor above — this call only publishes metadata.
  void registerManifest(WebManager* web);

  // Public so the FreeRTOS walker task (free function) can use the type.
  struct ArchiveJob {
    enum class State { Idle = 0, Walking, Ready, Streaming, Error };
    volatile State state{State::Idle};
    volatile bool cancelRequested{false};
    volatile uint32_t walkedCount{0};
    volatile uint32_t totalBytes{0};
    String path;          // volume-rooted path being archived
    String archiveName;   // suggested filename (without .tar)
    fs::FS* fs{nullptr};
    String error;
    std::vector<TarStream::Entry> entries;
    std::unique_ptr<TarStream> streamer;
  };

  // Per-file hash job for on-demand verify endpoint.
  struct HashJob {
    enum class State { Idle = 0, Running, Done, Error };
    volatile State state{State::Idle};
    volatile bool cancelRequested{false};
    volatile uint32_t bytesRead{0};
    volatile uint32_t total{0};
    String path;
    String fsPath;
    fs::FS* fs{nullptr};
    String sha256Hex;
    String crc32Hex;
    String error;
  };

 private:
  AsyncWebServer* _server;
  FileSystemManager* _manager;
  SecurityManager* _sec;

  // The streamed upload (one at a time; another gets 503 "busy"). Its bytes
  // go through an ESPRack::UploadPump (net/UploadPump.h): written as they
  // come while the flash write gate is free, else kept and written by the
  // FlashWriter task, one gate slot per 4 KB step; the answer comes once
  // the file is in place. Into <target>.partial, promoted at the end:
  //   finish 1: close   2: remove the old target (.corrupt on a hash
  //   mismatch)   3: rename .partial -> target (-> .corrupt: 422)
  // A client that goes keeps the .partial (purgeUploadDebris at boot);
  // a failed write removes it.
  struct Upload {
    FileSystemManager* mgr{nullptr};
    String target;  // volume-rooted; the bytes go to target + ".partial"
    File file;
    // the sink's (the FlashWriter task, or async_tcp while the gate is free)
    FsResult err{FsResult::OK};  // open / write / promote
    uint8_t step{0};             // finish calls done
    bool ioFailed{false};        // a write failed: abort removes the .partial
    // async_tcp's (the request's chunks), set before the final push
    bool sawFinal{false};
    bool incomplete{false};  // the body ended without the file part's end: keep the .partial, 500
    bool hashOk{true};
    // Optional integrity check supplied by client via X-File-SHA256 / X-File-CRC32 headers.
    bool hashEnabled{false};
    bool sha256Init{false};
    mbedtls_sha256_context sha256{};
    uint32_t crc{0};
    String expectedSha256;
    String expectedCrc32;
    String actualSha256;
    String actualCrc32;
    ~Upload() {
      if (sha256Init) mbedtls_sha256_free(&sha256);
    }
  };
  std::unique_ptr<Upload> _upload;  // the current or last one (its sink and answer use it)
  ESPRack::UploadPump _pump;
  static ESPRack::UploadSink uploadSink(Upload* u);
  static AsyncWebServerResponse* uploadResponse(AsyncWebServerRequest* request, const Upload& u,
                                                ESPRack::UploadResult r);
  // Refused before the pump took the request: answered now, marked so the
  // rest of its body and the done handler leave it alone.
  static void refuseUpload(AsyncWebServerRequest* request, int code, const char* msg);
  static void refuseUpload(AsyncWebServerRequest* request, int code, FsResult res);

  // GET handlers
  void handleSchema(AsyncWebServerRequest* request);
  void handleVolumes(AsyncWebServerRequest* request);
  void handleList(AsyncWebServerRequest* request);
  void handleStat(AsyncWebServerRequest* request);
  void handleDownload(AsyncWebServerRequest* request);
  void handleFormatStatus(AsyncWebServerRequest* request);
  void handleArchiveStatus(AsyncWebServerRequest* request);
  void handleArchiveDownload(AsyncWebServerRequest* request);

  // POST JSON handlers
  void handleMkdir(AsyncWebServerRequest* request, JsonVariant& json);
  void handleDelete(AsyncWebServerRequest* request, JsonVariant& json);
  void handleRename(AsyncWebServerRequest* request, JsonVariant& json);
  void handleFormat(AsyncWebServerRequest* request, JsonVariant& json);
  void handleUnmount(AsyncWebServerRequest* request, JsonVariant& json);
  void handleMount(AsyncWebServerRequest* request, JsonVariant& json);
  void handleArchivePrepare(AsyncWebServerRequest* request, JsonVariant& json);
  void handleArchiveCancel(AsyncWebServerRequest* request, JsonVariant& json);
  void handleHashStart(AsyncWebServerRequest* request, JsonVariant& json);
  void handleHashStatus(AsyncWebServerRequest* request);
  void handleHashCancel(AsyncWebServerRequest* request, JsonVariant& json);
  // A write the flash write gate put off (mkdir / delete / rename).
  void handleWriteStatus(AsyncWebServerRequest* request);
  void sendWriteResult(AsyncWebServerRequest* request, FsResult r, uint32_t op);

  ArchiveJob _archive;
  HashJob _hash;

  // Upload (multipart)
  void handleUploadDone(AsyncWebServerRequest* request);
  void handleUpload(AsyncWebServerRequest* request,
                    const String& filename,
                    size_t index,
                    uint8_t* data,
                    size_t len,
                    bool final);

  static void sendError(AsyncWebServerRequest* request, int code, FsResult res);
  static void sendError(AsyncWebServerRequest* request, int code, const char* msg);
  static void sendJson(AsyncWebServerRequest* request, int code, const JsonDocument& doc);
  static int httpCodeFor(FsResult r);
};

#endif  // FileSystemService_h
