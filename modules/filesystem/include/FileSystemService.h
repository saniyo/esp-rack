#ifndef FileSystemService_h
#define FileSystemService_h

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <mbedtls/sha256.h>

#include "FileSystemManager.h"
#include "SecurityManager.h"
#include "TarStream.h"

class WebManager;

#define FS_REST_BASE "/rest/fs"
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

class FileSystemService {
 public:
  FileSystemService(AsyncWebServer* server,
                    FileSystemManager* manager,
                    SecurityManager* securityManager);

  // Register this service's 28 endpoints with the WebManager so the frontend
  // can resolve URLs by role from /rest/uiManifest. Route handlers remain
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

  // current streamed upload (only one at a time under AsyncWebServer)
  File _uploadFile;
  bool _uploadFailed{false};
  String _uploadTarget;

  // Optional integrity check supplied by client via X-File-SHA256 / X-File-CRC32 headers.
  bool _hashEnabled{false};
  bool _sha256CtxInit{false};
  mbedtls_sha256_context _sha256Ctx{};
  uint32_t _crc{0};
  String _expectedSha256;
  String _expectedCrc32;

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
