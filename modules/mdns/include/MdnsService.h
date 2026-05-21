#pragma once
#ifndef MdnsService_h
#define MdnsService_h

// MdnsService — concrete IMdnsResolver impl wrapping arduino-esp32's
// ESPmDNS singleton.
//
// Lifecycle:
//   begin()       — first call to MDNS.begin() once WiFi STA has IP.
//                   Subscribes to WiFi events to re-init on reconnect
//                   (mDNS dies cleanly when WiFi drops; without
//                   re-init it stays silent after STA comes back).
//   loop()        — health check; if isActive() drifted false but
//                   WiFi is up, attempt re-init with backoff.
//   resolveHost() — synchronous MDNS.queryHost() with timeout.
//   browseService() — synchronous MDNS.queryService() snapshot.
//
// UI:
//   /rest/system/mdns                 — form schema for the System >
//                                       mDNS tab (status + browse +
//                                       manual resolve form)
//   action `mdns.resolveHost`         — query a hostname, flashes the
//                                       resolved IP in the response
//                                       message
//   action `mdns.refreshServices`     — re-runs a default browse over
//                                       the well-known service types
//                                       used by the framework
//                                       (_esprack._tcp, _https._tcp,
//                                       _http._tcp)
//   action `mdns.reinit`              — forced MDNS.end()+begin();
//                                       useful when something broke
//                                       the responder on the LAN.

#include <WiFi.h>
#include <ESPmDNS.h>
#include <vector>

#include <ESPAsyncWebServer.h>
#include <SecurityManager.h>
#include "IMdnsResolver.h"

class WebManager;

class MdnsService : public IMdnsResolver {
 public:
  // Server pointer is taken in the ctor (matches SystemStatus pattern);
  // the AsyncWebServer is what we hook the REST endpoint onto in
  // registerManifest. Module passes both from BuilderContext.
  explicit MdnsService(AsyncWebServer* server);

  // Adds the System > mDNS tab.
  void registerManifest(WebManager* web);

  void begin();
  void loop();

  // IMdnsResolver
  bool isActive() const override { return _active; }
  const char* hostname() const override { return _hostname.c_str(); }
  IPAddress resolveHost(const String& host,
                        uint32_t timeout_ms = 2000) override;
  std::vector<ServiceEntry> browseService(const String& service,
                                          const String& proto,
                                          uint32_t timeout_ms = 2000) override;

 private:
  // Build the System > mDNS form payload.
  void buildForm(JsonObject& root);
  void serveForm(AsyncWebServerRequest* request);

  // Bring MDNS up after WiFi got an IP. Idempotent — calling twice
  // performs MDNS.end() first to clear stale state.
  bool initResponder();

  // Compute the hostname we broadcast as. Today: DeviceIdentity::
  // canonical short ("ESPRackDemo-deb315c6") with non-alphanumerics
  // replaced by '-' (mDNS hostnames are constrained to LDH letters).
  String computeHostname() const;

  // Cached "last operator-initiated resolve" — surfaced as a row on
  // the mDNS tab so the operator sees the result without parsing the
  // snackbar text.
  struct LastResolve {
    String    host;
    IPAddress ip;
    uint32_t  whenMs{0};
  };
  LastResolve _lastResolve;

  // Cache of the most recent service browse, for the table view.
  // Refreshed on action `mdns.refreshServices` or implicitly on the
  // first form GET after WiFi up.
  std::vector<ServiceEntry> _browseCache;
  uint32_t _browseCacheMs{0};

  AsyncWebServer* _server{nullptr};
  String   _hostname;
  bool     _active{false};
  uint32_t _lastInitAttemptMs{0};
  uint32_t _initBackoffMs{2000};  // doubles up to 30s on persistent failure
  // True while a background `mdns-browse` task is running a
  // queryService(). Prevents back-to-back Refresh clicks from
  // spawning duplicate tasks while the first ~9 sec scan is
  // in flight.
  volatile bool _browseTaskRunning{false};
};

#endif  // MdnsService_h
