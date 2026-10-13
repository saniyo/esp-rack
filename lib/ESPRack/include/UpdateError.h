#pragma once
#ifndef ESPRACK_UPDATE_ERROR_H
#define ESPRACK_UPDATE_ERROR_H

// Why a state updater returned StateUpdateResult::ERROR (LIB-FEAT-001).
// The updater calls setUpdateError("GPIO35 reserved: octal PSRAM/flash");
// the REST write paths (feature POST /state, HttpPostEndpoint, rest.proxy)
// put it into the 400 problem's detail instead of the generic text.
//
// One slot, owned by the task that set it: a reason set on async_tcp is
// never reported to a request served by another task. Take it right after
// the update returns ERROR; clearUpdateError() before an update drops a
// stale one.

#include <ArduinoJson.h>
#include <stddef.h>

class AsyncWebServerRequest;

namespace ESPRack {

void setUpdateError(const char* reason);
void clearUpdateError();
// Copies the current task's reason into buf and clears it. false (buf = "")
// when this task set none.
bool takeUpdateError(char* buf, size_t len);

// The write paths' replies to an ERROR (shared, not per template instance):
// a 400 problem+json "update_rejected" with this task's reason as detail
// (or a generic text), and the rest.proxy body
// {"error":"update_rejected","detail":...}.
void sendUpdateRejected(AsyncWebServerRequest* request);
void writeUpdateRejected(JsonVariant out);

}  // namespace ESPRack

#endif  // ESPRACK_UPDATE_ERROR_H
