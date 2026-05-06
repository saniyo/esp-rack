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
};

#endif
