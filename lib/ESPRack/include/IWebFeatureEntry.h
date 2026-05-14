#ifndef IWebFeatureEntry_h
#define IWebFeatureEntry_h

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <SecurityManager.h>

struct WebTabSpec;

class IWebFeatureEntry {
 public:
  enum class Kind { Feature, Action, Status, Page };

  virtual ~IWebFeatureEntry() = default;

  virtual const char* id() const = 0;
  virtual Kind kind() const = 0;

  // Bind server routes. Called from WebManager::begin() after all entries are registered.
  virtual void registerEndpoints(AsyncWebServer* server, SecurityManager* sm) = 0;

  // Serialise this entry into a JsonObject for /rest/uiManifest.
  virtual void toJson(JsonObject& obj) const = 0;

  // Append a tab to this entry's tab list. Returns true if accepted.
  // Used by WebManager::addTabToFeature so compound features can be
  // filled in by multiple services at construction time instead of a
  // single aggregator declaring every tab in one place.
  virtual bool addTab(const WebTabSpec& /*tab*/) { return false; }

  // Phase 2 — in-process REST dispatch for Mothership rest.proxy.
  // Mothership receives a queued `rest.proxy{method,path,body}` action
  // from the server; instead of letting that loop back through the
  // device's AsyncWebServer (which would re-run JWT checks against a
  // request that came from the trusted mTLS check-in channel), we ask
  // each entry "do you own this (method, path) pair?". On match, the
  // entry populates out_status + out_body and returns true. Default
  // impl returns false so any custom IWebFeatureEntry that doesn't
  // know about the proxy mechanism stays inert (no security risk —
  // mothership-action call simply gets "404 not handled").
  //
  // method is uppercased ("GET"/"POST"/"PUT"/"DELETE"). path is the
  // exact restPath registered (no query string — caller strips). body
  // is the JSON body for POST/PUT (null/empty for GET). out_body is
  // a JsonVariant the caller pre-allocated against a sized document.
  virtual bool proxyDispatch(const char* /*method*/,
                              const char* /*path*/,
                              JsonVariant /*body*/,
                              int& /*out_status*/,
                              JsonVariant /*out_body*/) {
    return false;
  }
};

#endif
